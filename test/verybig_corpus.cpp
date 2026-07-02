// Corpus acceptance: runs verybig.lua end-to-end through the evaluator.
// Sets _soft = true to skip the >64k program tests (memory-intensive).

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "doctest.h"
#include "lua/state.h"

TEST_CASE("corpus: verybig.lua")
{
    std::string path = YUESHI_CORPUS_DIR "/lua-5.4.8-tests/verybig.lua";
    std::ifstream is(path, std::ios::binary);
    REQUIRE(is.good());
    std::string source{std::istreambuf_iterator<char>{is}, {}};
    source = "_soft = true\n" + source;

    ys::lua::State state;
    bool ok = false;
    try {
        state.run_string(source);
        ok = true;
    } catch (...) { ok = false; }
    CHECK(ok);
}
