#pragma once

// yueshi runtime value model + GC object graph.
//
// Two kinds of data, sharply separated:
//   - Scalars (nil/bool/int/float): pure by-value, no allocation, no identity,
//     no tracing. `1 == 1` is value equality, free.
//   - Objects (String/Table/Closure/Builtin/Environment): identity-bearing and
//     aliased ("a={}; b=a" is two references to one table). They are owned
//     EXCLUSIVELY by the Heap (a single intrusive linked list of GCObject);
//     every other holder keeps a NON-OWNING raw pointer. Reclamation is the
//     mark-sweep collector's job.
//
// shared_ptr appears nowhere: aliasing in Lua is many references to one object,
// not co-ownership. Designing the GC header in at the floor (rather than bolting
// GC on later) is exactly what makes cyclic structures (t1.x=t2;t2.x=t1) and
// escaping closures collectable purely from a root set — the reason this beats
// refcounting.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ys
{
    namespace lua
    {
        class Evaluator;   // forward; builtins receive it by reference
        struct FuncBody;   // forward (ast.h); a Closure references it, non-owning

        // -------------------------------------------------------------------
        // Intrusive GC header. Mirrors reference Lua's GCheader: every
        // collectable object starts with this so the Heap can (a) enumerate all
        // live objects via the intrusive `next` list, (b) dispatch the tracer
        // via `type`, and (c) run a stop-the-world mark-sweep via `marked`.
        // No virtual destructor on purpose: sweep() casts on `type` and deletes
        // the correct derived type, avoiding a vtable per object (matches
        // reference Lua's choice for GC'd objects).
        // -------------------------------------------------------------------
        enum class ObjType : std::uint8_t {
            String, Table, Closure, Builtin, Env
        };

        struct GCObject {
            GCObject* next;     // Heap's intrusive list (the single owner)
            ObjType   type;
            bool      marked;   // black during mark phase; reset by collect/sweep
        };

        // Forward: LuaValue holds pointers to these; they are defined below,
        // once LuaValue and LuaKey are complete (a pointer to an incomplete type
        // is fine, so the ordering works).
        struct String;
        struct Table;
        struct Closure;
        struct Builtin;
        struct Environment;

        // -------------------------------------------------------------------
        // LuaValue: the by-value tagged union. Holds NO ownership — object
        // pointers are non-owning (the Heap owns the pointee). All alternatives
        // are scalars or pointers, so LuaValue is trivially copyable and a
        // std::vector<LuaValue> needs no custom move.
        // -------------------------------------------------------------------
        struct LuaValue {
            enum class Tag : std::uint8_t {
                Nil, Bool, Int, Flt, Str, Table, Closure, Builtin
            };
            Tag tag{Tag::Nil};
            union Data {
                bool       b;
                long long  i;
                double     f;
                String*    s;
                Table*     t;
                Closure*   c;
                Builtin*   fn;
            } d{};   // value-initialized -> zero bytes (well-defined for Nil)

            LuaValue() = default;

            // Factories set the tag and exactly one union member.
            static LuaValue nil()                    { LuaValue v; v.tag = Tag::Nil;                    return v; }
            static LuaValue boolean(bool x)          { LuaValue v; v.tag = Tag::Bool;   v.d.b = x;     return v; }
            static LuaValue integer(long long x)     { LuaValue v; v.tag = Tag::Int;    v.d.i = x;     return v; }
            static LuaValue flt(double x)            { LuaValue v; v.tag = Tag::Flt;    v.d.f = x;     return v; }
            static LuaValue str(String* x)           { LuaValue v; v.tag = Tag::Str;    v.d.s = x;     return v; }
            static LuaValue table(Table* x)          { LuaValue v; v.tag = Tag::Table;  v.d.t = x;     return v; }
            static LuaValue closure(Closure* x)      { LuaValue v; v.tag = Tag::Closure;v.d.c = x;     return v; }
            static LuaValue builtin(Builtin* x)      { LuaValue v; v.tag = Tag::Builtin;v.d.fn = x;    return v; }

            bool is_nil() const noexcept      { return tag == Tag::Nil; }
            bool is_bool() const noexcept     { return tag == Tag::Bool; }
            bool is_int() const noexcept      { return tag == Tag::Int; }
            bool is_flt() const noexcept      { return tag == Tag::Flt; }
            bool is_number() const noexcept   { return is_int() || is_flt(); }
            bool is_str() const noexcept      { return tag == Tag::Str; }
            bool is_table() const noexcept    { return tag == Tag::Table; }
            bool is_closure() const noexcept  { return tag == Tag::Closure; }
            bool is_builtin() const noexcept  { return tag == Tag::Builtin; }
            bool is_callable() const noexcept { return is_closure() || is_builtin(); }

            bool       as_bool() const noexcept     { return d.b; }
            long long  as_int() const noexcept      { return d.i; }
            double     as_flt() const noexcept      { return d.f; }
            String*    as_str() const noexcept      { return d.s; }
            Table*     as_table() const noexcept    { return d.t; }
            Closure*   as_closure() const noexcept  { return d.c; }
            Builtin*   as_builtin() const noexcept  { return d.fn; }

            // If this value holds a GC object, return its GCObject* (for the
            // tracer); otherwise nullptr. Scalars are never traced.
            GCObject* as_gc() const noexcept;

            // Lua truthiness: only nil and false are falsy. (0/"" are truthy.)
            bool truthy() const noexcept
            {
                return !(is_nil() || (is_bool() && !d.b));
            }
        };

        using ValueVec = std::vector<LuaValue>;

        // -------------------------------------------------------------------
        // LuaKey: the hashable subset of a LuaValue, normalized. Integral floats
        // collapse to Int (so t[1] and t[1.0] share a slot); NaN is rejected at
        // insertion. Object values (table/closure/builtin) hash by identity
        // (their GCObject* pointer) — reference Lua permits them as keys.
        // -------------------------------------------------------------------
        struct LuaKey {
            enum class K : std::uint8_t { Int, Flt, Str, Bool, Ptr };
            K k{K::Int};
            long long   i{0};
            double      f{0.0};
            std::string s;
            bool        b{false};
            GCObject*   ptr{nullptr};

            LuaKey() = default;
        };

        bool operator==(const LuaKey& a, const LuaKey& b) noexcept;
        inline bool operator!=(const LuaKey& a, const LuaKey& b) noexcept { return !(a == b); }

        // Normalize a value to a key. Returns false (and leaves `out` untouched)
        // for nil and NaN, which cannot be table keys. Defined in heap.cpp.
        bool to_key(const LuaValue& v, LuaKey& out) noexcept;

    } // namespace lua
} // namespace ys

// std::hash<LuaKey> specialization — MUST be complete BEFORE the Table struct
// is defined, because std::unordered_map<LuaKey,_> (a Table member) requires
// a complete hash type at instantiation. Declared in namespace std as the
// standard requires.
namespace std
{
    template <>
    struct hash<ys::lua::LuaKey> {
        std::size_t operator()(const ys::lua::LuaKey& key) const noexcept
        {
            using K = ys::lua::LuaKey::K;
            // A Tag integral + a member hash, folded boost-style. The tag is
            // mixed in so distinct kinds never collide.
            std::size_t h = std::hash<unsigned>{}(static_cast<unsigned>(key.k));
            auto fold = [&](std::size_t x) {
                h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            };
            switch (key.k) {
            case K::Int:  fold(std::hash<long long>{}(key.i));    break;
            case K::Flt:  fold(std::hash<double>{}(key.f));       break;
            case K::Str:  fold(std::hash<std::string>{}(key.s));  break;
            case K::Bool: fold(std::hash<bool>{}(key.b));         break;
            case K::Ptr:  fold(std::hash<const void*>{}(key.ptr)); break;
            }
            return h;
        }
    };
} // namespace std

namespace ys
{
    namespace lua
    {

        // -------------------------------------------------------------------
        // Object types. All derive GCObject and are Heap-owned. Their ctors
        // value-initialize the base (GCObject{}) so members start zeroed; the
        // Heap's allocator then stamps next/type/marked.
        // -------------------------------------------------------------------
        using BuiltinFn = ValueVec (*)(Evaluator&, ValueVec);

        struct String : GCObject {
            std::string data;
            explicit String(std::string d) : GCObject{}, data(std::move(d)) {}
        };

        struct Table : GCObject {
            std::unordered_map<LuaKey, LuaValue> hash;
            // Per-table metatable (Lua 5.4 §2.4). Non-owning: the Heap owns it
            // like every other GC object. Traced in heap.cpp trace(). nil in
            // the common case; populated by setmetatable().
            Table* metatable{nullptr};
            Table() : GCObject{} {}
        };

        struct Closure : GCObject {
            const FuncBody* body;   // non-owning; the AST (parser) owns it
            Environment*    env;    // non-owning; the Heap owns it
            Table*          env_table;   // _ENV upvalue (captured globals table)
            bool            is_vararg;
            // Per-closure metatable. Seldom used but supported (Lua allows
            // __call on functions via the per-type function metatable; we model
            // it per-value for uniformity with tables).
            Table*          metatable{nullptr};
            Closure(const FuncBody* b, Environment* e, Table* et, bool v)
                : GCObject{}, body(b), env(e), env_table(et), is_vararg(v) {}
        };

        struct Builtin : GCObject {
            std::string name;
            BuiltinFn   fn;
            Builtin(std::string n, BuiltinFn f) : GCObject{}, name(std::move(n)), fn(f) {}
        };

        // A scope is itself a GC object: a closure can escape its defining frame
        // (`return function() return x end`), so a scope's lifetime is not the
        // C stack frame. Making Environment traceable (vars + parent + varargs)
        // is what lets escaping AND cyclic env<->closure pairs be collected.
        struct Environment : GCObject {
            Environment* parent;                        // non-owning
            Table*       env_table;                     // _ENV for this scope
            std::unordered_map<std::string, LuaValue> vars;
            std::unordered_set<std::string> consts;     // <const> enforcement
            std::vector<LuaValue> varargs;              // '...' for this frame
            explicit Environment(Environment* p)
                : GCObject{}, parent(p),
                  env_table(p ? p->env_table : nullptr) {}
        };

        // as_gc: defined here, after all four object types are complete (the
        // static_casts need the derived types to confirm the base relationship).
        inline GCObject* LuaValue::as_gc() const noexcept
        {
            switch (tag) {
            case Tag::Str:     return static_cast<GCObject*>(d.s);
            case Tag::Table:   return static_cast<GCObject*>(d.t);
            case Tag::Closure: return static_cast<GCObject*>(d.c);
            case Tag::Builtin: return static_cast<GCObject*>(d.fn);
            default:           return nullptr;
            }
        }

    } // namespace lua
} // namespace ys
