#define BOOST_TEST_MODULE yueshi_lua_lex_test
#include <boost/test/unit_test.hpp>
#include "lua/lex.h"

using namespace ys::lua;

BOOST_AUTO_TEST_CASE(test_tokens) {
    Tokenizer t{"print"};
    auto tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    BOOST_CHECK_EQUAL(std::get<2>(tk.value().info) , "print");
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_EOS));
}

BOOST_AUTO_TEST_CASE(test_tokens1) {
    std::string input = R"(print ("Hello"
    ))";
    Tokenizer t{input};
    auto tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    BOOST_CHECK_EQUAL(std::get<2>(tk.value().info) , "print");
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , '(');
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_STRING));
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , ')');
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_EOS));
}

BOOST_AUTO_TEST_CASE(test_multiline) {
    std::string input{R"(AA
    BB
    '123')"};
    Tokenizer t{input};
    auto tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    BOOST_CHECK_EQUAL(std::get<2>(tk.value().info) , "AA");
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_NAME));
    BOOST_CHECK_EQUAL(std::get<2>(tk.value().info) , "BB");
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_STRING));
    tk = t.next();
    BOOST_CHECK(tk);
    BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_EOS));
}

BOOST_AUTO_TEST_CASE(test_long_str) {
    {
        std::string input{R"([[adf
asdf]])"};
        Tokenizer t{input};
        auto tk = t.next();
        BOOST_CHECK(tk);
        BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_STRING));
        BOOST_CHECK_EQUAL(std::get<2>(tk.value().info).size(), input.size()-4);
    }
    {
            std::string input{R"(123 0.5 [==[adf
asdf]==])"};
            Tokenizer t{input};
            auto tk = t.next();
            BOOST_CHECK(tk);
            BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_INT));
            BOOST_CHECK_EQUAL(std::get<0>(tk.value().info) , 123);
            tk = t.next();
            BOOST_CHECK(tk);
            BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_FLT));
            BOOST_CHECK_EQUAL(std::get<1>(tk.value().info) , 0.5);
            tk = t.next();
            BOOST_CHECK(tk);
            BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_STRING));
            BOOST_CHECK_EQUAL(std::get<2>(tk.value().info).size(), 8);
    }
}

BOOST_AUTO_TEST_CASE(test_comment) {
    {
        std::string input{R"(--[[adf
asdf]])"};
        Tokenizer t{input};
        auto tk = t.next();
        BOOST_CHECK(tk);
        BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_EOS));
    }
}