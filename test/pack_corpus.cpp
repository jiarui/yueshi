// string.pack/unpack/packsize corpus acceptance: run the official Lua 5.4.8
// test suite's tpack.lua end-to-end through the evaluator and assert it
// completes without error (any failed assert/checkerror throws a LuaError).
// This is the high-leverage gate for M3.2.5: tpack.lua alone exercises ~420
// runtime assertions covering every directive, endian, alignment, error
// message, and edge case.
//
// tpack.lua depends on: string.pack/unpack/packsize, string.find/rep/sub/
// format, math.maxinteger/mininteger, print, pcall, ipairs, bitwise ops,
// //, #. All of these work in yueshi today, so the file runs unmodified.

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "lua/evaluator.h"
#include "lua/lex.h"
#include "lua/parser.h"
#include "lua/value.h"

using namespace ys::lua;

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream is(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{is}, {}};
}

} // namespace

TEST_CASE("corpus: tpack.lua runs clean end-to-end")
{
    namespace fs = std::filesystem;
    fs::path tpack = fs::path{YUESHI_CORPUS_DIR} / "tpack.lua";
    REQUIRE_MESSAGE(fs::exists(tpack),
                    "tpack.lua not found at " << tpack.string());

    std::string src = slurp(tpack.string());
    REQUIRE_FALSE_MESSAGE(src.empty(), "tpack.lua is empty or unreadable");

    // Lex
    Tokenizer tok{src};
    auto tokens = tok.tokenize();
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
        REQUIRE_MESSAGE(tokens[i].id != -1,
                        "lex stall in tpack.lua at token " << i);

    // Parse
    Parser p{std::move(tokens)};
    auto ast_opt = p.parse();
    REQUIRE_MESSAGE(ast_opt, "tpack.lua failed to parse");
    auto errs = p.take_errors();
    REQUIRE_MESSAGE(errs.empty(),
                    "tpack.lua produced parse diagnostics: " << errs[0]);

    // Run
    Heap heap;
    std::ostringstream capture;   // suppress tpack.lua's print() output
    Evaluator ev{heap, capture};
    try {
        auto results = ev.run(std::move(*ast_opt));
        (void)results;
    }
    catch (const LuaError& e) {
        // A failed assert or checkerror inside tpack.lua propagates here.
        FAIL("tpack.lua assertion failed: " << e.what());
    }
}
