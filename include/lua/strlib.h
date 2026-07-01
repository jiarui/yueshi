#pragma once

// yueshi string library (M3.1): basic functions, string.format, the pattern
// engine wrappers (find/match/gmatch/gsub), and string.pack/unpack/packsize.
// Also installs the per-type string metatable so ("hi"):upper() works.
//
// install_string_lib() creates the `string` table in globals, populates it
// with all builtins, creates the per-type string metatable (with __index =
// the string table), and stores the metatable on the Evaluator.

namespace ys
{
    namespace lua
    {
        class Evaluator;

        // Create and install the string library + per-type string metatable.
        // Called once from Evaluator::install_builtins().
        void install_string_lib(Evaluator& ev);
    }
}
