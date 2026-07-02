// Corpus acceptance: runs the official utf8.lua end-to-end through the
// evaluator. The full library (len/offset/codepoint/char/codes + lax mode
// + charpattern) is exercised.

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "doctest.h"
#include "lua/state.h"

TEST_CASE("corpus: utf8.lua")
{
    std::string path = YUESHI_CORPUS_DIR "/lua-5.4.8-tests/utf8.lua";
    ys::lua::State state;
    CHECK(state.run_file(path));
}
