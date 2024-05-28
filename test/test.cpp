#define BOOST_TEST_MODULE yueshi_lua_lex_test
#include <boost/test/unit_test.hpp>
#include "lua/lex.h"

using namespace ys::lua;

// BOOST_AUTO_TEST_CASE(test_tokens) {
//     Tokenizer t{"print"};
//     auto tk = t.next();
//     BOOST_CHECK(tk);
//     BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_NAME));
//     BOOST_CHECK_EQUAL(std::get<2>(tk.value().info) , "print");
//     tk = t.next();
//     BOOST_CHECK(tk);
//     BOOST_CHECK_EQUAL(tk.value().id , static_cast<Token::TokenIDType>(TokenID::TK_EOS));
// }

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