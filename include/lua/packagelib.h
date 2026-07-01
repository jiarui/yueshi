#pragma once

// Lua 5.4 `package` library + `require` builtin (M3.5-D).
// Implements the module loading mechanism: package.path/cpath resolution,
// package.loaded cache, package.preload override, and the searcher chain.
// C-module loading (dlopen) is stubbed — yueshi has no C FFI.

#include "lua/evaluator.h"

namespace ys
{
    namespace lua
    {
        void install_package_lib(Evaluator& ev);
    } // namespace lua
} // namespace ys
