#pragma once

// Static goto/label scope analysis. Runs between parse and eval to enforce
// Lua 5.4's compile-time goto restrictions (§3.3.2, §3.5):
//   R1: a goto must target a visible label (same function, enclosing blocks)
//   R2: a goto must not jump into the scope of a local variable
//   R3: a label must not duplicate a visible label of the same name
//   R4: break must be inside a loop
//
// The pass mimics Lua's nactvar (active-local count) model: each goto and
// label records the scope depth at its position; R2 checks
//   goto.nactvar < label.nactvar  →  "jumps into the scope of local 'X'"
// with the end-of-block exemption (a label that is the last non-void
// statement of its block has its nactvar reset to the enclosing scope).
//
// On violation, throws LuaError with the exact Lua message substring (so
// goto.lua's `string.find(msg, m)` assertions match).

#include <cstddef>
#include <string_view>

#include "lua/ast.h"

namespace ys
{
    namespace lua
    {
        // Check a chunk AST for goto/label scope violations. `source` is the
        // original source text (for byte-offset → line-number conversion in
        // error messages). Throws LuaError on the first violation found.
        void check_goto_scopes(const AstNode& root, std::string_view source);
    } // namespace lua
} // namespace ys
