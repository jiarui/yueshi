#include "doctest.h"
#include "lua/lex.h"

using namespace ys::lua;

namespace {
// Shorthand for the underlying-type id of a TokenID.
constexpr Token::TokenIDType id_of(TokenID t) {
    return static_cast<Token::TokenIDType>(t);
}
} // namespace

TEST_CASE("test_tokens")
{
    std::string input = "input";
    auto toks = Tokenizer{input}.tokenize();
    REQUIRE(toks.size() == 2);
    CHECK(toks[0].id == id_of(TokenID::TK_NAME));
    CHECK(std::get<2>(toks[0].info) == input);
    CHECK(toks[1].id == id_of(TokenID::TK_EOS));
}

TEST_CASE("test_tokens_paren_string")
{
    std::string input = R"(print ("Hello"
    ))";
    auto toks = Tokenizer{input}.tokenize();
    // print ( "Hello" )
    REQUIRE(toks.size() == 5);
    CHECK(toks[0].id == id_of(TokenID::TK_NAME));
    CHECK(std::get<2>(toks[0].info) == "print");
    CHECK(toks[1].id == '(');
    CHECK(toks[2].id == id_of(TokenID::TK_STRING));
    CHECK(std::get<2>(toks[2].info) == "Hello");
    CHECK(toks[3].id == ')');
    CHECK(toks[4].id == id_of(TokenID::TK_EOS));
}

TEST_CASE("test_multiline")
{
    std::string input{R"(AA
    BB
    '123')"};
    auto toks = Tokenizer{input}.tokenize();
    REQUIRE(toks.size() == 4);
    CHECK(toks[0].id == id_of(TokenID::TK_NAME));
    CHECK(std::get<2>(toks[0].info) == "AA");
    CHECK(toks[1].id == id_of(TokenID::TK_NAME));
    CHECK(std::get<2>(toks[1].info) == "BB");
    CHECK(toks[2].id == id_of(TokenID::TK_STRING));
    CHECK(std::get<2>(toks[2].info) == "123");
    CHECK(toks[3].id == id_of(TokenID::TK_EOS));
}

TEST_CASE("test_long_str")
{
    SUBCASE("level-0 long string")
    {
        std::string input{R"([[adf
asdf]])"};
        auto toks = Tokenizer{input}.tokenize();
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_STRING));
        // long-bracket body is not escape-decoded; it's the raw bytes between
        // the brackets (minus the leading newline and the closing ]]).
        CHECK(std::get<2>(toks[0].info).size() == input.size() - 4);
        CHECK(toks[1].id == id_of(TokenID::TK_EOS));
    }
    SUBCASE("mixed int / float / level-2 long string")
    {
        std::string input{R"(123 0.5 [==[adf
asdf]==])"};
        auto toks = Tokenizer{input}.tokenize();
        REQUIRE(toks.size() == 4);
        CHECK(toks[0].id == id_of(TokenID::TK_INT));
        CHECK(std::get<0>(toks[0].info) == 123);
        CHECK(toks[1].id == id_of(TokenID::TK_FLT));
        CHECK(std::get<1>(toks[1].info) == 0.5);
        CHECK(toks[2].id == id_of(TokenID::TK_STRING));
        CHECK(std::get<2>(toks[2].info).size() == 8);
        CHECK(toks[3].id == id_of(TokenID::TK_EOS));
    }
    SUBCASE("UTF-8 string literal")
    {
        std::string input{R"("été")"};
        auto toks = Tokenizer{input}.tokenize();
        REQUIRE(toks.size() == 2);
        CHECK(toks[0].id == id_of(TokenID::TK_STRING));
        CHECK(std::get<2>(toks[0].info).size() == 5);
        CHECK(toks[1].id == id_of(TokenID::TK_EOS));
    }
}

TEST_CASE("test_comment")
{
    SUBCASE("long-bracket comment")
    {
        std::string input{R"(--[[adf
asdf]])"};
        auto toks = Tokenizer{input}.tokenize();
        REQUIRE(toks.size() == 1);
        CHECK(toks[0].id == id_of(TokenID::TK_EOS));
    }
}

TEST_CASE("test_files")
{
    auto toks = Tokenizer{"print"}.tokenize();
    REQUIRE(toks.size() == 2);
    CHECK(toks[0].id == id_of(TokenID::TK_NAME));
    CHECK(std::get<2>(toks[0].info) == "print");
    CHECK(toks[1].id == id_of(TokenID::TK_EOS));
}
