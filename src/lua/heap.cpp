#include "lua/heap.h"

#include <vector>

#include "lua/ast.h"   // FuncBody (Closure references it by pointer)

namespace ys
{
    namespace lua
    {
        // -------------------------------------------------------------------
        // Lifetime + intrusive list management
        // -------------------------------------------------------------------
        Heap::~Heap()
        {
            GCObject* cur = m_head;
            while (cur) {
                GCObject* nxt = cur->next;
                free_one(cur);
                cur = nxt;
            }
            m_head = nullptr;
            m_count = 0;
        }

        // free_one is a non-template helper in the .cpp; it casts on `type` and
        // deletes the correct derived type. (No virtual destructor on GCObject,
        // matching reference Lua's GC-object choice and avoiding a vtable per
        // object. If you add an object type, add its case here.)
        void Heap::free_one(GCObject* o) noexcept
        {
            switch (o->type) {
            case ObjType::String:  delete static_cast<String*>(o);      break;
            case ObjType::Table:   delete static_cast<Table*>(o);       break;
            case ObjType::Closure: delete static_cast<Closure*>(o);     break;
            case ObjType::Builtin: delete static_cast<Builtin*>(o);     break;
            case ObjType::Env:     delete static_cast<Environment*>(o); break;
            }
        }

        void Heap::link(GCObject* o, ObjType type) noexcept
        {
            o->type   = type;
            o->marked = false;
            o->next   = m_head;
            m_head    = o;
            ++m_count;
        }

        // -------------------------------------------------------------------
        // Typed allocators
        // -------------------------------------------------------------------
        String* Heap::make_string(std::string s)
        {
            auto* o = new String(std::move(s));
            link(o, ObjType::String);
            maybe_collect();
            return o;
        }

        Table* Heap::make_table()
        {
            auto* o = new Table();
            link(o, ObjType::Table);
            maybe_collect();
            return o;
        }

        Closure* Heap::make_closure(const FuncBody* body, Environment* env,
                                    bool is_vararg)
        {
            auto* o = new Closure(body, env, is_vararg);
            link(o, ObjType::Closure);
            maybe_collect();
            return o;
        }

        Builtin* Heap::make_builtin(std::string name, BuiltinFn fn)
        {
            auto* o = new Builtin(std::move(name), fn);
            link(o, ObjType::Builtin);
            // Builtins are roots-ish; no auto-collect here (they are allocated
            // once at State setup, outside any expression scope).
            return o;
        }

        Environment* Heap::make_env(Environment* parent)
        {
            auto* o = new Environment(parent);
            link(o, ObjType::Env);
            maybe_collect();
            return o;
        }

        // -------------------------------------------------------------------
        // Trigger: between statements only (AllocationScope suppresses mid-
        // expression). During a collection, suppress re-entry.
        // -------------------------------------------------------------------
        void Heap::maybe_collect()
        {
            // m_threshold == 0 disables auto-GC (used by tests). Otherwise the
            // evaluator's AllocationScope sets m_threshold to SIZE_MAX while it
            // holds; this guard is a second line of defense.
            if (m_marking)                  return;
            if (m_threshold == 0)           return;   // GC disabled
            if (m_count < m_threshold)      return;
            // No roots available here: the Heap cannot self-collect (it does
            // not know the live environment chain). Real triggers are issued by
            // the evaluator at statement boundaries via collect(roots). The
            // threshold model keeps amortized cost bounded; when the threshold
            // trips mid-expression it is deferred to the next boundary.
        }

        // -------------------------------------------------------------------
        // Mark phase
        // -------------------------------------------------------------------
        void Heap::reset_marks() noexcept
        {
            for (GCObject* o = m_head; o; o = o->next) o->marked = false;
        }

        void Heap::push_mark(GCObject* o) noexcept
        {
            if (o && !o->marked) {
                o->marked = true;
                m_worklist.push_back(o);
            }
        }

        void Heap::drain_worklist()
        {
            while (!m_worklist.empty()) {
                GCObject* o = m_worklist.back();
                m_worklist.pop_back();
                trace(o, MarkVisitor{*this});
            }
        }

        // -------------------------------------------------------------------
        // Sweep phase: delete+unlink white objects, reset black->white.
        // -------------------------------------------------------------------
        std::size_t Heap::sweep()
        {
            std::size_t swept = 0;
            GCObject** pp = &m_head;
            while (*pp) {
                GCObject* cur = *pp;
                if (!cur->marked) {
                    *pp = cur->next;     // unlink
                    free_one(cur);
                    --m_count;
                    ++swept;
                } else {
                    cur->marked = false;  // reset for next cycle
                    pp = &cur->next;
                }
            }
            return swept;
        }

        // -------------------------------------------------------------------
        // trace: the object-graph walker (template defined here, where all
        // object struct layouts are complete).
        // -------------------------------------------------------------------
        template <class Emit>
        void trace(GCObject* o, Emit&& emit)
        {
            switch (o->type) {
            case ObjType::String:
            case ObjType::Builtin:
                break;   // leaves — no outgoing edges
            case ObjType::Table: {
                auto* t = static_cast<Table*>(o);
                for (const auto& [k, v] : t->hash)
                    if (GCObject* c = v.as_gc()) emit(c);
                if (t->metatable)
                    emit(static_cast<GCObject*>(t->metatable));
                break;
            }
            case ObjType::Closure: {
                auto* c = static_cast<Closure*>(o);
                if (c->env) emit(static_cast<GCObject*>(c->env));
                if (c->metatable)
                    emit(static_cast<GCObject*>(c->metatable));
                break;
            }
            case ObjType::Env: {
                auto* e = static_cast<Environment*>(o);
                if (e->parent) emit(static_cast<GCObject*>(e->parent));
                for (const auto& [k, v] : e->vars)
                    if (GCObject* c = v.as_gc()) emit(c);
                for (const LuaValue& v : e->varargs)
                    if (GCObject* c = v.as_gc()) emit(c);
                break;
            }
            }
            (void)sizeof(Emit);  // silence unused-emit when a branch has none
        }

        // -------------------------------------------------------------------
        // Key normalization + equality (used by Table operations)
        // -------------------------------------------------------------------
        bool to_key(const LuaValue& v, LuaKey& out) noexcept
        {
            switch (v.tag) {
            case LuaValue::Tag::Nil:
                return false;                       // nil cannot be a key
            case LuaValue::Tag::Bool:
                out.k = LuaKey::K::Bool; out.b = v.d.b; return true;
            case LuaValue::Tag::Int:
                out.k = LuaKey::K::Int; out.i = v.d.i; return true;
            case LuaValue::Tag::Flt: {
                double f = v.d.f;
                if (f != f) return false;           // NaN: not a key
                // Integral floats collapse to an Int key: t[1] == t[1.0].
                long long as_i = static_cast<long long>(f);
                if (static_cast<double>(as_i) == f) {
                    out.k = LuaKey::K::Int; out.i = as_i; return true;
                }
                out.k = LuaKey::K::Flt; out.f = f; return true;
            }
            case LuaValue::Tag::Str:
                out.k = LuaKey::K::Str; out.s = v.d.s->data; return true;
            case LuaValue::Tag::Table:
                out.k = LuaKey::K::Ptr; out.ptr = static_cast<GCObject*>(v.d.t); return true;
            case LuaValue::Tag::Closure:
                out.k = LuaKey::K::Ptr; out.ptr = static_cast<GCObject*>(v.d.c); return true;
            case LuaValue::Tag::Builtin:
                out.k = LuaKey::K::Ptr; out.ptr = static_cast<GCObject*>(v.d.fn); return true;
            }
            return false;
        }

        bool operator==(const LuaKey& a, const LuaKey& b) noexcept
        {
            if (a.k != b.k) return false;
            switch (a.k) {
            case LuaKey::K::Int:  return a.i == b.i;
            case LuaKey::K::Flt:  return a.f == b.f;
            case LuaKey::K::Str:  return a.s == b.s;
            case LuaKey::K::Bool: return a.b == b.b;
            case LuaKey::K::Ptr:  return a.ptr == b.ptr;
            }
            return false;
        }

    } // namespace lua
} // namespace ys
