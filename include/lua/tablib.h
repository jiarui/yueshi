#pragma once

// yueshi table library (M3.2): insert/remove/concat/pack/unpack/move/sort.
// Mirrors the strlib layout: install_table_lib() creates the `table` table in
// globals and populates it. table.sort dispatches through the public
// Evaluator::call_value so a user comparator's metamethods are honored.

namespace ys
{
    namespace lua
    {
        class Evaluator;

        // Create and install the table library. Called once from
        // Evaluator::install_builtins().
        void install_table_lib(Evaluator& ev);
    }
}
