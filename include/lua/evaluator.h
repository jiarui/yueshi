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
        // render `chunk:line:col: <msg>` via a peg::SourceMap. M2.3: also
        // carries an optional error OBJECT (any LuaValue) — when error() is
        // called with a non-string value, pcall/xpcall must return that exact
        // object, not its string rendering. For all error sites other than
        // error(), the object is nil and pcall returns what() as a string.
        class LuaError : public std::runtime_error {
        public:
            LuaError(std::string what, std::size_t src_off)
                : std::runtime_error(std::move(what)), m_off(src_off) {}
            std::size_t offset() const noexcept { return m_off; }
            // The error object: non-nil only for error() with a non-string
            // value. pcall/xpcall return this; falls back to what() as a string.
            const LuaValue& obj() const noexcept { return m_obj; }
            void obj(LuaValue v) noexcept { m_obj = v; }
        private:
            std::size_t m_off;
            LuaValue    m_obj{LuaValue::nil()};
        };

        // Non-local control flow carried out of statement evaluation. Normal
        // completion is Flow::Normal; loops catch Break; call frames catch
        // Return and extract its values; eval_block catches Goto and resumes at
        // the matching Label (or propagates it outward to be caught by an
        // enclosing block, or reported as "no visible label" at a frame/chunk
        // boundary in call_value/run).
        enum class Flow : std::uint8_t { Normal, Break, Return, Goto };
        struct Control {
            Flow flow{Flow::Normal};
            ValueVec vals;          // populated only for Return
            std::string label;      // populated only for Goto
            std::size_t off{0};     // Goto's source offset (for diagnostics)
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

            // The global table (_G / the root _ENV). All builtins and stdlib
            // tables are registered here. Per-scope _ENV tables are derived
            // from this root (see Environment::env_table).
            Table& globals() noexcept { return *m_G; }

            // Register the M2.0 builtins into globals(). Called once at
            // construction; idempotent.
            void install_builtins();

            // Convert a value to a string value, consulting __tostring if the
            // value has one (used by the tostring/print builtins). Allocates the
            // result String on the heap. `off` is attached to any error raised.
            LuaValue stringify(const LuaValue& v, std::size_t off);

            // Call a callable value (closure/builtin, or anything with __call)
            // with args. Public so builtins like pcall/xpcall/table.sort can
            // drive function calls.
            ValueVec call_value(const LuaValue& f, ValueVec args, std::size_t off);

            // Per-type string metatable accessor (M3.1). strlib installs
            // __index = string-lib-table so ("hi"):upper() works.
            Table*& string_metatable() noexcept { return m_string_mt; }

            // Retain an AST root so closures created by load() can safely
            // reference its FuncBody nodes. The AST is not GC'd (Closure::body
            // is a raw pointer), so it must live for the Evaluator's lifetime.
            // Returns a stable const FuncBody* into the retained tree.
            const FuncBody* retain_chunk_ast(AstNode chunk_root);

        private:
            Heap&          m_heap;
            std::ostream*  m_out;
            Table*         m_G;             // _G: the root _ENV table
            Table*         m_string_mt{nullptr};  // per-type string metatable (M3.1)
            std::size_t    m_depth{0};
            const peg::SourceMap* m_map{nullptr};  // optional, for error locations

            // Per-block label cache (M2.2): maps a Block* to {name -> index in
            // stmts}. Built lazily on first eval_block entry, then preserved for
            // the run so re-entered loop/function bodies don't rescan. Cleared
            // at the top of run() so successive chunks can't observe stale
            // pointers (a freed AST would dangle). Last-wins on duplicate labels
            // (matches the runtime-only v1 stance; Lua rejects at compile time).
            struct LabelCache {
                std::unordered_map<std::string, std::size_t> m;
                bool built{false};
            };
            std::unordered_map<const Block*, LabelCache> m_labels;

            // AST roots from load()/dofile(). These are kept alive because
            // Closure::body is a raw pointer (not GC'd). Each entry is a
            // synthesized FuncBody wrapping the parsed chunk's Block.
            std::vector<AstNode> m_loaded_chunks;

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
            void index_set(const LuaValue& obj, std::string_view key,
                           LuaValue v, std::size_t off);

            // --- Metatable support (M2.1) ---
            // The metatable of a table/closure value, or nullptr. Scalars and
            // strings have no metatable in M2.1 (string metatables are M3).
            Table* metatable_of(const LuaValue& v) noexcept;
            // Look up a metamethod `ev` (e.g. "__add") on v's metatable. Raw
            // lookup (no recursion), returns nil if absent. Allocation-free:
            // builds a temporary LuaKey, not a String object.
            LuaValue getmetamethod(const LuaValue& v, std::string_view ev);
            // For binary ops: Lua checks the left operand's metamethod first,
            // then the right's. Returns the first present, else nil.
            LuaValue getmetamethod_bin(const LuaValue& a, const LuaValue& b,
                                       std::string_view ev);
            // Call a metamethod value and return its first result (nil if it
            // returns nothing). The caller has already verified `mm` is present.
            LuaValue call_metamethod(const LuaValue& mm, ValueVec args,
                                     std::size_t off);
            // Equality/ordering WITH metamethods. raw paths for
            // numbers/strings/nil/bool; __eq (table/closure pairs not raw-equal),
            // __lt, and __le (with the `not (b<a)` fallback via __lt).
            bool eq_meta(const LuaValue& a, const LuaValue& b, std::size_t off);
            bool lt_meta(const LuaValue& a, const LuaValue& b, std::size_t off);
            bool le_meta(const LuaValue& a, const LuaValue& b, std::size_t off);
        };

    } // namespace lua
} // namespace ys
