// Corpus acceptance: runs vararg.lua end-to-end through the evaluator.
// vararg.lua tests varargs, select, table.pack/unpack — no M4 deps.

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
}

TEST_CASE("corpus: vararg.lua")
{
    std::string path = YUESHI_CORPUS_DIR "/lua-5.4.8-tests/vararg.lua";
    ys::lua::State state;
    CHECK(state.run_file(path));
}
