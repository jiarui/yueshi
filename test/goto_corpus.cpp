// goto corpus acceptance: runs the M3.5-runnable portions of goto.lua
// end-to-end through the evaluator (lex + parse + goto_check + eval).
//
// The original goto.lua requires the debug library (M4) for upvalue-identity
// assertions and the <close> attribute (M4) for the final test section.
// This runner uses a stripped fixture (test/corpus/m3.5/goto_runtime.lua)
// that removes those sections but preserves all compile-time goto checks
// (R1-R4) and the runtime goto behavior.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "doctest.h"

#include "lua/state.h"

namespace
{
    std::string slurp(const char* path)
    {
        std::ifstream is(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{is}, {}};
    }

#ifndef YUESHI_CORPUS_DIR
#define YUESHI_CORPUS_DIR "."
#endif
}

TEST_CASE("goto corpus: stripped goto.lua runtime + compile checks")
{
    std::string path = YUESHI_CORPUS_DIR "/m3.5/goto_runtime.lua";
    std::string source = slurp(path.c_str());
    REQUIRE(!source.empty());

    ys::lua::State state;

    bool ok = state.run_file(path);
    CHECK(ok);
    // If run_file returns true, the chunk completed without error.
    // The print'OK' at the end of goto_runtime.lua confirms all
    // assertions passed (any failure would throw via assert()).
}
