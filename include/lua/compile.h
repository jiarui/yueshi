#pragma once

// Shared lex+parse entry point. Used by State (production) and by the
// load()/dofile() builtins (which need to compile source strings at runtime).
// Extracting this into a free function avoids giving the Evaluator a
// back-pointer to State.

#include <optional>
#include <string>
#include <vector>

#include "lua/ast.h"

namespace ys
{
    namespace lua
    {
        struct ParseResult {
            std::optional<AstNode> ast;
            std::vector<std::string> errors;
            // Convenience: did the compile succeed?
            explicit operator bool() const noexcept { return ast.has_value(); }
        };

        // Lex + parse a source string. The returned AstNode (if present) owns
        // its entire subtree (move-only). On failure, `errors` holds the
        // diagnostic strings and `ast` is std::nullopt.
        ParseResult compile_source(std::string source);
    } // namespace lua
} // namespace ys
