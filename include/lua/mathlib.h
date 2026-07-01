#pragma once

// yueshi math library (M3.3): constants (pi/huge/maxinteger/mininteger) plus
// the <cmath> wrappers and math.random/randomseed. math.random needs
// interpreter-resident state (the PRNG); install_math_lib wires it up via the
// evaluator so the seeded state lives for the run.

namespace ys
{
    namespace lua
    {
        class Evaluator;

        // Create and install the math library. Called once from
        // Evaluator::install_builtins().
        void install_math_lib(Evaluator& ev);
    }
}
