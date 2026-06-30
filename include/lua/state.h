#pragma once

// yueshi interpreter State: owns the GC Heap and drives lex → parse → evaluate
// for a source chunk. This is the production entry point (used by the yueshi
// CLI); the Evaluator is the engine, and State is the owner + driver.

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "lua/ast.h"      // AstNode
#include "lua/evaluator.h"
#include "lua/heap.h"
#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        class State {
        public:
            State();

            // Run a source string: tokenize, parse, evaluate. Returns the
            // chunk's return values (empty if it fell off the end). Errors are
            // thrown as LuaError; the caller (e.g. the CLI) renders them.
            ValueVec run_string(const std::string& source);

            // Read a file and run it. Returns true on clean completion, false
            // on a lex/parse error (with the diagnostics written to the State's
            // error stream). Runtime LuaError is caught and printed as
            // `lua: <msg>`, returning false.
            bool run_file(const std::string& path);

            // Output sink for print() (defaults to std::cout).
            std::ostream& out() noexcept { return *m_out; }
            void set_out(std::ostream& o) noexcept { m_out = &o; }

            // The owned heap + evaluator.
            Heap& heap() noexcept { return m_heap; }

        private:
            Heap m_heap;
            std::ostream* m_out;
            Evaluator m_eval;

            // Tokenize + parse a source string; on success returns the AST
            // root, on failure returns std::nullopt and fills `errors`.
            std::optional<AstNode> parse_string(const std::string& source,
                                                std::vector<std::string>& errors);
        };
    } // namespace lua
} // namespace ys
