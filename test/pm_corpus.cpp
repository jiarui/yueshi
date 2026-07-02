// Corpus acceptance: runs pm.lua with _soft=true (skips the 300KB big-string
// section which is slow under ASan instrumentation, though it passes clean
// in a normal build).
//
// Tests the full pattern engine: find/match/gmatch/gsub + captures,
// back-refs, %b balanced, %f frontier, charpattern, UTF-8 patterns.

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "doctest.h"
#include "lua/state.h"

TEST_CASE("corpus: pm.lua")
{
    std::string path = YUESHI_CORPUS_DIR "/lua-5.4.8-tests/pm.lua";
    std::ifstream is(path, std::ios::binary);
    REQUIRE(is.good());
    std::string source{std::istreambuf_iterator<char>{is}, {}};
    // _soft=true skips the 300KB big-string stress tests (lines 260-270).
    // These pass in normal builds but are pathologically slow under ASan
    // (every byte access on a 300K string is instrumented).
    source = "_soft = true\n" + source;

    ys::lua::State state;
    bool ok = false;
    try {
        state.run_string(source);
        ok = true;
    } catch (...) { ok = false; }
    CHECK(ok);
}
