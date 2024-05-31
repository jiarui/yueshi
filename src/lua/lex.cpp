#include "lua/lex.h"
#include <type_traits>
#include <string>
#include <string_view>
#include <iostream>
using namespace peg;
using namespace ys::lua;

#define STR_ELEMENT(p) #p

const static std::vector<const char *> tk_type_str = {
    STR_ELEMENT(TokenID::TK_AND),
    STR_ELEMENT(TokenID::TK_BREAK),
    STR_ELEMENT(TokenID::TK_ELSE),
    STR_ELEMENT(TokenID::TK_ELSEIF),
    STR_ELEMENT(TokenID::TK_FALSE),
    STR_ELEMENT(TokenID::TK_FOR),
    STR_ELEMENT(TokenID::TK_FUNCTION),
    STR_ELEMENT(TokenID::TK_IN),
    STR_ELEMENT(TokenID::TK_LOCAL),
    STR_ELEMENT(TokenID::TK_NIL),
    STR_ELEMENT(TokenID::TK_NOT),
    STR_ELEMENT(TokenID::TK_OR),
    STR_ELEMENT(TokenID::TK_REPEAT),
    STR_ELEMENT(TokenID::TK_RETURN),
    STR_ELEMENT(TokenID::TK_THEN),
    STR_ELEMENT(TokenID::TK_TRUE),
    STR_ELEMENT(TokenID::TK_UNTIL),
    STR_ELEMENT(TokenID::TK_WHILE),
    STR_ELEMENT(TokenID::TK_IDIV),
    STR_ELEMENT(TokenID::TK_CONCAT),
    STR_ELEMENT(TokenID::TK_DOTS),
    STR_ELEMENT(TokenID::TK_EQ),
    STR_ELEMENT(TokenID::TK_GE),
    STR_ELEMENT(TokenID::TK_LE),
    STR_ELEMENT(TokenID::TK_NE),
    STR_ELEMENT(TokenID::TK_SHL),
    STR_ELEMENT(TokenID::TK_SHR),
    STR_ELEMENT(TokenID::TK_DBCOLON),
    STR_ELEMENT(TokenID::TK_EOS),
    STR_ELEMENT(TokenID::TK_FLT),
    STR_ELEMENT(TokenID::TK_INT),
    STR_ELEMENT(TokenID::TK_NAME),
    STR_ELEMENT(TokenID::TK_STRING)
};

#undef STR_ELEMENT

const std::map<std::string_view, TokenID> ys::lua::str2tkid = {
    {"and", TokenID::TK_AND},
    {"break", TokenID::TK_BREAK},
    {"else", TokenID::TK_ELSE},
    {"elseif", TokenID::TK_ELSEIF},
    {"end", TokenID::TK_END},
    {"false", TokenID::TK_FALSE},
    {"for", TokenID::TK_FOR},
    {"function", TokenID::TK_FUNCTION},
    {"goto", TokenID::TK_GOTO},
    {"if", TokenID::TK_IF},
    {"in", TokenID::TK_IN},
    {"local", TokenID::TK_LOCAL},
    {"nil", TokenID::TK_NIL},
    {"not", TokenID::TK_NOT},
    {"or", TokenID::TK_OR},
    {"repeat", TokenID::TK_REPEAT},
    {"return", TokenID::TK_RETURN},
    {"then", TokenID::TK_THEN},
    {"true", TokenID::TK_TRUE},
    {"until", TokenID::TK_UNTIL},
    {"while", TokenID::TK_WHILE},
    {"//", TokenID::TK_IDIV},
    {"..", TokenID::TK_CONCAT},
    {"...", TokenID::TK_DOTS},
    {"==", TokenID::TK_EQ},
    {">=", TokenID::TK_GE},
    {"<=", TokenID::TK_LE},
    {"~=", TokenID::TK_NE},
    {"<<", TokenID::TK_SHL},
    {">>", TokenID::TK_SHR},
    {"::", TokenID::TK_DBCOLON},
};

std::ostream& operator<<(std::ostream& s, const Token& t) {
    auto id = static_cast<typename std::underlying_type<TokenID>::type>(t.id);
    auto index = id - UCHAR_MAX;
    s<<"Token Type"<< id << ':' << tk_type_str[index]<<std::endl;
    return s;
}



bool Token::operator==(const Token& rhs) {
    if(id != rhs.id){
        return false;
    }
    else if(id < static_cast<Token::TokenIDType>(TokenID::TK_FLT)) {
        return true;
    }
    else {
        return info == rhs.info;             
    }
}

