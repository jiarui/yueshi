// Corpus acceptance: runs sort.lua end-to-end through the evaluator.
// Sets _soft = true to use the 5000-element limit (instead of 50000).

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "doctest.h"
#include "lua/state.h"

TEST_CASE("corpus: sort.lua")
{
    std::string path = YUESHI_CORPUS_DIR "/lua-5.4.8-tests/sort.lua";
    std::ifstream is(path, std::ios::binary);
    REQUIRE(is.good());
    std::string source{std::istreambuf_iterator<char>{is}, {}};
    // Prepend _soft = true to reduce array sizes from 50000 to 5000.
    source = "_soft = true\n" + source;

    ys::lua::State state;
    bool ok = false;
    try {
        state.run_string(source);
        ok = true;
    } catch (...) { ok = false; }
    CHECK(ok);
}
