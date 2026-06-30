#pragma once

// yueshi Heap: the single owner of all GC objects + a stop-the-world mark-sweep
// collector. This is the "GC considered at the beginning" layer: every object
// the evaluator allocates lives here, and reclamation is reachability-based
// from a root set the evaluator supplies — never refcounting, so cycles
// (t1.x=t2; t2.x=t1) and escaping closures collect correctly.
//
// Tracing is a single free function, not a vtable: trace(obj, emit) walks an
// object's children and calls emit(child_gc*) for each. The collector marks
// the transitive closure of the roots, then sweeps the intrusive list,
// unlinking/deleting anything still white.
//
// Trigger model: collect only at statement boundaries, never mid-expression
// (see AllocationScope in evaluator.cpp). The threshold doubles after each
// collect, giving amortized O(1) per allocation; state.collect() forces a pass.

#include <cstddef>
#include <string>
#include <utility>

#include "lua/value.h"   // GCObject, object types, LuaValue, LuaKey

namespace ys
{
    namespace lua
    {
        class Heap {
        public:
            Heap() = default;
            // Non-copyable: it owns raw object lifetimes.
            Heap(const Heap&) = delete;
            Heap& operator=(const Heap&) = delete;
            // Frees every object still on the intrusive list.
            ~Heap();

            // Typed allocators. Each links the new object into the live list,
            // stamps type/marked, bumps the count, and (in collect_points() mode)
            // may trigger a collection — but the evaluator holds an
            // AllocationScope across each expression to suppress that, so a
            // collection never tears down a half-built value.
            String*      make_string(std::string s);
            Table*       make_table();
            Closure*     make_closure(const FuncBody* body, Environment* env,
                                      bool is_vararg);
            Builtin*     make_builtin(std::string name, BuiltinFn fn);
            Environment* make_env(Environment* parent);

            // The number of live objects. For tests + diagnostics.
            std::size_t live_count() const noexcept { return m_count; }

            // Force a collection now, marking the transitive closure of `roots`.
            // `roots` is called with a visitor and must invoke it once per root
            // GCObject*/LuaValue (the collector's mark phase uses the same
            // visitor). Returns the number of objects swept.
            template <class RootFn>
            std::size_t collect(RootFn roots)
            {
                m_marking = true;   // suppress self-trigger mid-collect
                // Phase 1 — mark: grey each root, then trace to fixpoint via
                // a worklist. (A simple explicit stack avoids C recursion on
                // deep object graphs.)
                reset_marks();
                roots(MarkVisitor{*this});
                drain_worklist();
                // Phase 2 — sweep: delete unmarked, unlink from list.
                std::size_t swept = sweep();
                (void)swept;
                m_marking = false;
                if (m_count > 0)
                    m_threshold = m_count * 2;   // grow headroom
                else
                    m_threshold = 1024;
                return swept;
            }

        private:
            // Marking machinery.
            struct MarkVisitor {
                Heap& h;
                // Push a single GCObject* root (if non-null) onto the worklist.
                void operator()(GCObject* o) const { if (o) h.push_mark(o); }
                // Push the object (if any) referenced by a value root.
                void operator()(const LuaValue& v) const
                {
                    if (GCObject* o = v.as_gc()) h.push_mark(o);
                }
            };

            friend struct MarkVisitor;

            GCObject*   m_head{nullptr};   // intrusive list of ALL live objects
            std::size_t m_count{0};
            std::size_t m_threshold{1024};
            bool        m_marking{false};  // true during collect() to suppress triggers

            // The mark worklist (transitive closure frontier).
            std::vector<GCObject*> m_worklist;

            void link(GCObject* o, ObjType type) noexcept;
            void free_one(GCObject* o) noexcept;   // cast on type, delete derived
            void maybe_collect();

            void reset_marks() noexcept;
            void push_mark(GCObject* o) noexcept;   // white->black, enqueue
            void drain_worklist();
            std::size_t sweep();
        };

        // trace: enumerate the child GCObject*s of `o`, calling emit(child) for
        // each. Scalars have none; tables yield their value slots' objects;
        // closures yield their env; environments yield parent + varargs +
        // each var's object. (Env.vars keys are strings-by-value, not objects.)
        template <class Emit>
        void trace(GCObject* o, Emit&& emit);

    } // namespace lua
} // namespace ys
