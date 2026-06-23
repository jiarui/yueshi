#include "doctest.h"
#include "lua/lex.h"

using namespace ys::lua;

TEST_CASE("test_tokens")
{
    std::string input = "input";
    Tokenizer t{input};
    auto tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    CHECK(std::get<2>(tk.value().info) == input);
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_EOS));
}

TEST_CASE("test_tokens_paren_string")
{
    std::string input = R"(print ("Hello"
    ))";
    Tokenizer t{input};
    auto tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    CHECK(std::get<2>(tk.value().info) == "print");
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == '(');
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_STRING));
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == ')');
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_EOS));
}

TEST_CASE("test_multiline")
{
    std::string input{R"(AA
    BB
    '123')"};
    Tokenizer t{input};
    auto tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    CHECK(std::get<2>(tk.value().info) == "AA");
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    CHECK(std::get<2>(tk.value().info) == "BB");
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_STRING));
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_EOS));
}

TEST_CASE("test_long_str")
{
    SUBCASE("level-0 long string")
    {
        std::string input{R"([[adf
asdf]])"};
        Tokenizer t{input};
        auto tk = t.next();
        REQUIRE(tk.has_value());
        CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_STRING));
        CHECK(std::get<2>(tk.value().info).size() == input.size() - 4);
    }
    SUBCASE("mixed int / float / level-2 long string")
    {
        std::string input{R"(123 0.5 [==[adf
asdf]==])"};
        Tokenizer t{input};
        auto tk = t.next();
        REQUIRE(tk.has_value());
        CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_INT));
        CHECK(std::get<0>(tk.value().info) == 123);
        tk = t.next();
        REQUIRE(tk.has_value());
        CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_FLT));
        CHECK(std::get<1>(tk.value().info) == 0.5);
        tk = t.next();
        REQUIRE(tk.has_value());
        CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_STRING));
        CHECK(std::get<2>(tk.value().info).size() == 8);
    }
    SUBCASE("UTF-8 string literal")
    {
        std::string input{R"("été")"};
        Tokenizer t{input};
        auto tk = t.next();
        REQUIRE(tk.has_value());
        CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_STRING));
        CHECK(std::get<2>(tk.value().info).size() == 5);
    }
}

TEST_CASE("test_comment")
{
    SUBCASE("long-bracket comment")
    {
        std::string input{R"(--[[adf
asdf]])"};
        Tokenizer t{input};
        auto tk = t.next();
        REQUIRE(tk.has_value());
        CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_EOS));
    }
}

TEST_CASE("test_files")
{
    Tokenizer t{"print"};
    auto tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    CHECK(std::get<2>(tk.value().info) == "print");
    tk = t.next();
    REQUIRE(tk.has_value());
    CHECK(tk.value().id == static_cast<Token::TokenIDType>(TokenID::TK_EOS));
}
