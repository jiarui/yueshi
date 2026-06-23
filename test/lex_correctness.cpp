// Lexer correctness / regression suite. Each TEST_CASE pins one bug from the
// audit so a regression fails a named, specific case rather than a vague
// symptom. Cases use the audit's exact exposure inputs.
#include "doctest.h"
#include "lua/lex.h"
#include <sstream>
#include <string>

using namespace ys::lua;

namespace {
constexpr Token::TokenIDType id_of(TokenID t) {
    return static_cast<Token::TokenIDType>(t);
}
// Lex `input` and require that it produced no error sentinel (id == -1) before
// the trailing EOS. Returns the full token vector.
std::vector<Token> lex_ok(const std::string& input) {
    auto toks = Tokenizer{input}.tokenize();
    // No token before EOS may be the error sentinel.
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        CHECK(toks[i].id != -1);
    }
    return toks;
}
} // namespace

// ---------------------------------------------------------------------------
// H1: leading sign must NOT be part of a numeric literal. `3-2` is three
// tokens, `-3` is two. Subtraction adjacent to a digit used to be destroyed.
// ---------------------------------------------------------------------------
TEST_CASE("H1 signed_literal_is_unary_op") {
    SUBCASE("subtraction 3-2") {
        auto toks = lex_ok("3-2");
        REQUIRE(toks.size() == 4); // 3 - 2 EOS
        CHECK(toks[0].id == id_of(TokenID::TK_INT));
        CHECK(std::get<0>(toks[0].info) == 3);
        CHECK(toks[1].id == '-');
        CHECK(toks[2].id == id_of(TokenID::TK_INT));
        CHECK(std::get<0>(toks[2].info) == 2);
        CHECK(toks[3].id == id_of(TokenID::TK_EOS));
    }
    SUBCASE("unary minus -3") {
        auto toks = lex_ok("-3");
        REQUIRE(toks.size() == 3);
        CHECK(toks[0].id == '-');
        CHECK(toks[1].id == id_of(TokenID::TK_INT));
        CHECK(std::get<0>(toks[1].info) == 3);
    }
    SUBCASE("negated hex float -0x1p4") {
        auto toks = lex_ok("-0x1p4");
        REQUIRE(toks.size() == 3);
        CHECK(toks[0].id == '-');
        CHECK(toks[1].id == id_of(TokenID::TK_FLT));
        CHECK(std::get<1>(toks[1].info) == 16.0);
    }
}

// ---------------------------------------------------------------------------
// H2b: hex integers must parse as integers with base 16. They used to fall
// through to a base-10 float parse and become TK_FLT(0.0).
// ---------------------------------------------------------------------------
TEST_CASE("H2b hex_integer") {
    SUBCASE("0xFF") {
        auto toks = lex_ok("0xFF");
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_INT));
        CHECK(std::get<0>(toks[0].info) == 255);
    }
    SUBCASE("0xDEADBEEF (needs >32 bits)") {
        auto toks = lex_ok("0xDEADBEEF");
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_INT));
        CHECK(std::get<0>(toks[0].info) == 0xDEADBEEFLL); // 3735928559
    }
    SUBCASE("uppercase 0X1a") {
        auto toks = lex_ok("0X1a");
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_INT));
        CHECK(std::get<0>(toks[0].info) == 26);
    }
}

// ---------------------------------------------------------------------------
// H2a: an out-of-range decimal integer re-reads as a float (Lua semantics),
// NOT silently clamped to INT_MAX.
// ---------------------------------------------------------------------------
TEST_CASE("H2a int_overflow_becomes_float") {
    auto toks = lex_ok("99999999999999999999"); // ~1e20, overflows long long
    REQUIRE(toks.size() == 2);
    CHECK(toks[0].id == id_of(TokenID::TK_FLT));
    CHECK(std::get<1>(toks[0].info) == doctest::Approx(1e20));
}

// ---------------------------------------------------------------------------
// H3: hex floats must parse via from_chars(chars_format::hex). They used to
// become TK_FLT(0.0).
// ---------------------------------------------------------------------------
TEST_CASE("H3 hex_float") {
    SUBCASE("0x1.8p3 == 12.0") {
        auto toks = lex_ok("0x1.8p3");
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_FLT));
        CHECK(std::get<1>(toks[0].info) == doctest::Approx(12.0));
    }
    SUBCASE("0x1p4 == 16.0") {
        auto toks = lex_ok("0x1p4");
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_FLT));
        CHECK(std::get<1>(toks[0].info) == doctest::Approx(16.0));
    }
    SUBCASE("negative exponent 0xA23p-4") {
        auto toks = lex_ok("0xA23p-4");
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_FLT));
        // 0xA23 = 2595; p-4 => /16 => 162.1875
        CHECK(std::get<1>(toks[0].info) == doctest::Approx(2595.0 / 16.0));
    }
}

// ---------------------------------------------------------------------------
// H4a: short-string escape sequences must be DECODED, not stored raw.
// ---------------------------------------------------------------------------
TEST_CASE("H4a escape_decode") {
    SUBCASE("backslash-n") {
        auto toks = lex_ok("\"a\\nb\"");
        REQUIRE(toks.size() == 2);
        const auto& s = std::get<2>(toks[0].info);
        CHECK(s.size() == 3);
        CHECK(s == std::string{"a\nb"});
    }
    SUBCASE("control escapes") {
        auto toks = lex_ok("\"\\a\\b\\f\\n\\r\\t\\v\"");
        REQUIRE(toks.size() == 2);
        const auto& s = std::get<2>(toks[0].info);
        CHECK(s == std::string{"\a\b\f\n\r\t\v", 7});
    }
    SUBCASE("decimal escape \\65 == 'A'") {
        auto toks = lex_ok("\"\\65\"");
        REQUIRE(toks.size() == 2);
        const auto& s = std::get<2>(toks[0].info);
        CHECK(s == "A");
    }
    SUBCASE("hex escape \\xAB") {
        auto toks = lex_ok("\"\\xAB\"");
        REQUIRE(toks.size() == 2);
        const auto& s = std::get<2>(toks[0].info);
        CHECK(s.size() == 1);
        CHECK(static_cast<unsigned char>(s[0]) == 0xAB);
    }
    SUBCASE("unicode escape \\u{3B1} == α (UTF-8 CE B1)") {
        auto toks = lex_ok("\"\\u{3B1}\"");
        REQUIRE(toks.size() == 2);
        const auto& s = std::get<2>(toks[0].info);
        CHECK(s.size() == 2);
        CHECK(static_cast<unsigned char>(s[0]) == 0xCE);
        CHECK(static_cast<unsigned char>(s[1]) == 0xB1);
    }
    SUBCASE("\\z skips following whitespace") {
        auto toks = lex_ok("\"a\\z   b\"");
        REQUIRE(toks.size() == 2);
        const auto& s = std::get<2>(toks[0].info);
        CHECK(s == "ab");
    }
}

// ---------------------------------------------------------------------------
// H4b: the escapable quote inside a double-quoted string is '"', not '\''.
// "a\"b" used to be un-lexable.
// ---------------------------------------------------------------------------
TEST_CASE("H4b escaped_double_quote") {
    auto toks = lex_ok("\"a\\\"b\"");
    REQUIRE(toks.size() == 2);
    CHECK(std::get<2>(toks[0].info) == "a\"b");
}

// ---------------------------------------------------------------------------
// H5a: "do" is a keyword. It used to be missing from str2tkid and lexed as a
// name, breaking every do...end block.
// ---------------------------------------------------------------------------
TEST_CASE("H5a do_keyword") {
    auto toks = lex_ok("do end");
    REQUIRE(toks.size() == 3);
    CHECK(toks[0].id == id_of(TokenID::TK_DO));
    CHECK(toks[1].id == id_of(TokenID::TK_END));
    CHECK(toks[2].id == id_of(TokenID::TK_EOS));
    // Sanity: every Lua keyword round-trips through str2tkid.
    for (auto kw : {"and", "break", "do", "else", "elseif", "end", "false",
                    "for", "function", "goto", "if", "in", "local", "nil",
                    "not", "or", "repeat", "return", "then", "true", "until",
                    "while"}) {
        CHECK(str2tkid.count(kw) == 1);
    }
}

// ---------------------------------------------------------------------------
// H5b: operator<< index math. It used to be off-by-one AND tk_type_str was
// missing entries, so streaming TK_NAME/TK_EOS/TK_FLT read out of bounds (UB).
// ---------------------------------------------------------------------------
TEST_CASE("H5b token_debug_print_no_oob") {
    auto check = [](TokenID tid) {
        Token t;
        t.id = id_of(tid);
        std::ostringstream os;
        os << t; // must not crash / OOB
        CHECK(os.str().find("Token Type") != std::string::npos);
    };
    check(TokenID::TK_AND);    // first enum entry
    check(TokenID::TK_DO);     // previously missing from tk_type_str
    check(TokenID::TK_GOTO);   // previously missing
    check(TokenID::TK_IF);     // previously missing
    check(TokenID::TK_CONCAT); // multi-char operator
    check(TokenID::TK_FLT);    // used to OOB
    check(TokenID::TK_INT);    // used to OOB
    check(TokenID::TK_NAME);   // used to OOB
    check(TokenID::TK_STRING); // last enum entry, used to OOB
    check(TokenID::TK_EOS);    // used to OOB
}

// ---------------------------------------------------------------------------
// H8.1: a short comment may run to EOF with no trailing newline. It used to
// require a terminating linebreak and fall through to '-' '-' name.
// ---------------------------------------------------------------------------
TEST_CASE("H8.1 eof_comment") {
    SUBCASE("no trailing newline") {
        auto toks = lex_ok("-- comment");
        REQUIRE(toks.size() == 1);
        CHECK(toks[0].id == id_of(TokenID::TK_EOS));
    }
    SUBCASE("comment then code after newline") {
        auto toks = lex_ok("-- c\nx");
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_NAME));
        CHECK(std::get<2>(toks[0].info) == "x");
        CHECK(toks[1].id == id_of(TokenID::TK_EOS));
    }
}

// ---------------------------------------------------------------------------
// B (diagnostics): on a lexing error (e.g. unterminated string), take_error()
// must surface a "file:line:col: error: expected ..." message instead of the
// old silent-sentinel behaviour.
// ---------------------------------------------------------------------------
TEST_CASE("diagnostics unterminated_string") {
    // Unterminated short string: the closing quote never comes.
    Tokenizer t{"\"abc"};
    auto toks = t.tokenize();
    // The scan should have hit an error sentinel (id == -1) before EOS.
    bool found_err = false;
    for (const auto& tk : toks) {
        if (tk.id == -1) { found_err = true; break; }
    }
    CHECK(found_err);
    auto diag = t.take_error();
    REQUIRE(diag.has_value());
    CHECK(diag->find(": error:") != std::string::npos);
}

// ---------------------------------------------------------------------------
// B (source ranges): tokens carry byte offsets the parser can map to line:col.
// ---------------------------------------------------------------------------
TEST_CASE("source_ranges populated") {
    // "ab cd" -> positions: ab=[0,2), space, cd=[3,5)
    auto toks = lex_ok("ab cd");
    REQUIRE(toks.size() == 3);
    CHECK(toks[0].start == 0);
    CHECK(toks[0].end == 2);
    CHECK(toks[1].start == 3);
    CHECK(toks[1].end == 5);
    CHECK(toks[2].id == id_of(TokenID::TK_EOS));
}
