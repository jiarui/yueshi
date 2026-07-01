#pragma once

// yueshi os library (M3.4-C): thin wrappers over POSIX/CRT.
// time/clock/difftime/date/getenv/exit/execute/remove/rename/setlocale/tmpname.

namespace ys
{
    namespace lua
    {
        class Evaluator;

        // Create and install the os library. Called once from
        // Evaluator::install_builtins().
        void install_os_lib(Evaluator& ev);
    }
}
