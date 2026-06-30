#pragma once

// yueshi tree-walking evaluator: AstNode -> LuaValue.
//
// The evaluator is the bridge between the move-only AST (parser output) and the
// GC'd runtime value model (value.h/heap.h). It walks the tree, allocating
// objects on the Heap and binding names in Environments. Control flow uses a
// plain Control struct (no exceptions for break/return); runtime *errors* throw
// LuaError, caught at the top level.
//
// Ownership: the Evaluator does not own the AST or the Heap — it borrows both
// for the duration of run(). The Heap is owned by the State; the AST root is
// moved into run() by value (the caller yields ownership for the run).

#include <cstddef>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

#include "lua/ast.h"
#include "lua/heap.h"
#include "lua/value.h"
#include "peglib.h"   // peg::SourceMap (for error location rendering)

namespace ys
{
    namespace lua
    {
        // A runtime error: type errors, bad index, arity, non-function call,
        // depth limit, etc. Carries a source byte offset so the top level can
        // render `chunk:line:col: <msg>` via a peg::SourceMap.
        class LuaError : public std::runtime_error {
        public:
            LuaError(std::string what, std::size_t src_off)
                : std::runtime_error(std::move(what)), m_off(src_off) {}
            std::size_t offset() const noexcept { return m_off; }
        private:
            std::size_t m_off;
        };

        // Non-local control flow carried out of statement evaluation. Normal
        // completion is Flow::Normal; loops catch Break; call frames catch
        // Return and extract its values.
        enum class Flow : std::uint8_t { Normal, Break, Return };
        struct Control {
            Flow flow{Flow::Normal};
            ValueVec vals;   // populated only for Return
        };

        class Evaluator {
        public:
            // Borrows the Heap (allocations) and the output stream (for print).
            // The Heap must outlive the Evaluator.
            explicit Evaluator(Heap& heap, std::ostream& out);

            // Execute a chunk (the AST root). The main chunk is a vararg
            // function whose `...` is empty in M2.0. Returns the chunk's
            // return values (empty if it fell off the end).
            ValueVec run(AstNode root);

            // Output sink accessors (for tests that inject a stringstream).
            std::ostream& out() noexcept { return *m_out; }

            Heap& heap() noexcept { return m_heap; }

            // The global environment (the chunk's enclosing scope).
            Environment& globals() noexcept { return *m_globals; }

            // Register the M2.0 builtins into globals(). Called once at
            // construction; idempotent.
            void install_builtins();

        private:
            Heap&          m_heap;
            std::ostream*  m_out;
            Environment*   m_globals;       // non-owning; Heap owns it
            std::size_t    m_depth{0};
            const peg::SourceMap* m_map{nullptr};  // optional, for error locations

            // Statement/expression evaluation (defined in evaluator.cpp).
            Control  eval_block(const Block&, Environment*);
            // Evaluate a Box that holds a Block (the common body-shape: the
            // parser wraps statement blocks in an AstNode variant).
            Control  eval_block_box(const Box& body, Environment* env)
            { return body ? eval_block(get<Block>(*body), env) : Control{}; }
            Control  eval_block_box(const std::optional<Box>& body, Environment* env)
            { return body ? eval_block_box(*body, env) : Control{}; }
            Control  eval_stat(const AstNode&, Environment*);
            ValueVec eval_expr(const AstNode&, Environment*);          // multires
            LuaValue eval_scalar(const AstNode&, Environment*);        // one value

            // Evaluate an explist/arglist: all-but-last produce one value each;
            // the LAST expands via multires if it is a call/method/vararg.
            ValueVec eval_explist(const std::vector<Box>& exprs, Environment* env);

            // Bind/resolve helpers.
            LuaValue lookup(Environment* env, std::string_view name);
            void     assign(Environment* env, std::string_view name,
                            LuaValue v, std::size_t off);

            // Assign to an lvalue (Name / Index / Field). Implemented in Step 6
            // for Index/Field; Name goes through assign().
            void assign_target(const AstNode& target, Environment* env,
                               LuaValue v, std::size_t off);

            // Call a callable value (closure or builtin) with args. Implemented
            // in Step 5; declared here so generic-for can drive the iterator.
            ValueVec call_value(const LuaValue& f, ValueVec args, std::size_t off);

            // Evaluate a Call node: func + args -> result values.
            ValueVec eval_call(const Call& c, Environment* env);
            // Table/string indexing by a string key (for Field access and
            // method lookup). Step 6 handles the table store; reads are here.
            LuaValue index_get(const LuaValue& obj, std::string_view key,
                               std::size_t off);
            LuaValue index_get(const LuaValue& obj, const LuaValue& key,
                               std::size_t off);
            // Table store: t[key] = v. Computes the border cache invalidation
            // lazily (# recomputes when needed).
            void index_set(const LuaValue& obj, const LuaValue& key,
                           LuaValue v, std::size_t off);
        };

    } // namespace lua
} // namespace ys
