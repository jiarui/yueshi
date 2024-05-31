#include "lua/lex.h"
#include <type_traits>
#include <string>
#include <charconv>
#include <string_view>
#include <iostream>
using namespace peg;
using namespace ys::lua;

using namespace peg;

namespace lexconv
{
    template<typename Context>
    struct Rules {
        using value_type = typename Context::ValueType;
        using LuaRule = Rule<Context>;
        inline static Rule<Context> ops = terminalSeq("...") |
            terminalSeq("..") |
            terminalSeq("<<") |
            terminalSeq(">>") |
            terminalSeq("//") |
            terminalSeq("==") |
            terminalSeq("~=") |
            terminalSeq("<=") |
            terminalSeq(">=") |
            terminalSeq("::") |
            '+' | '-' | '*' | '/' | '%' | '^' | '#' | '&' | '~' | '|' | '<' | '>' | '=' | '(' | ')' | '{' | '}' | '[' | ']' | ';' | ':'| ',' | '.';
        inline static auto WS = +terminal<value_type>(std::set({' ', '\f', '\t', '\v'}));
        inline static auto not_linebreak = terminal<value_type>([](value_type c){return c!='\n';});
        inline static auto name_start = terminal<value_type>([](value_type c){return std::isalpha(c) || c == '_';});
        inline static auto name_cont = terminal<value_type>([](value_type c){return std::isalnum(c) || c=='_';});
        inline static Rule<Context> name =  name_start >> *name_cont;
        inline static auto linebreak = terminalSeq<value_type>("\r\n") | terminal('\n');
        inline static auto digit = terminal<value_type>('0', '9');
        inline static auto xdigit = terminal<value_type>([](char c){return std::isxdigit(c);});
        inline static auto pos_or_neg = (terminal<value_type>('+') | '-');
        inline static auto fractional = -pos_or_neg >> ((*digit >> '.' >> +digit) | (+digit >> '.' >> *digit));
        inline static auto decimal = -pos_or_neg >> +digit;
        inline static auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >> -('.' >> +xdigit) >> -((terminal('p') | 'P') >> +decimal);
        inline static auto expotent = -pos_or_neg >> +digit;
        inline static Rule<Context> numeral = hexdecimal | ((fractional | decimal) >> -(terminal('e') | 'E') >> -(decimal));

        inline static auto common_escape_code = terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' | (terminal('\\') >>'\\'>>'n')| ('z' >> WS) | (3 * digit) | (2 * xdigit) | (terminal('u') >> '{' >> *xdigit >> '}') ;
        inline static Rule<Context> single_escape_code = terminal('\\') >> ( common_escape_code | '\'' );
        inline static Rule<Context> double_escape_code = terminal('\\') >> ( common_escape_code | '\'' );
        inline static auto single_no_escape_code = terminal<char>([](char c){return c != '\'';});
        inline static auto double_no_escape_code = terminal<char>([](char c){return c != '"';});
        inline static auto string_single_quote = '\'' >> *(single_escape_code | single_no_escape_code) >> '\'';
        inline static auto string_double_quote = '"' >> *(double_escape_code | double_no_escape_code) >> '"';
        inline static Rule<Context> long_bracket_start = '[' >> *terminal('=') >> '[';
        inline static Rule<Context> comment_long_bracket_start = '[' >> *terminal('=') >> '[';
        inline static Rule<Context> string_literal = string_single_quote | string_double_quote | long_bracket_start;
        
        inline static Rule<Context> comment = terminal('-') >> '-' >> (comment_long_bracket_start | (*not_linebreak >> linebreak));
        inline static auto cut_ = cut<Context>();
        inline static Rule<Context> token = comment | (WS >> cut_) | (numeral >> cut_) | (name >> cut_) | (string_literal >> cut_) | ops | linebreak;
    };

} // namespace lexconv
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

const std::map<std::string_view, TokenID> str2tkid = {
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

template<>
Tokenizer<std::span<const typename std::string::value_type>>::Tokenizer(const std::string& input) : m_context(input) {
    lexconv::Rules<Context>::comment_long_bracket_start.setAction([this](decltype(m_context)& c, decltype(m_context)::MatchRange m) {
        int level = m.end() - m.begin() - 2;
        assert(level>=0);
        auto end_mark = ']' >> (level * terminal('=')) >> ']';
        Rule<> long_bracket_end = end_mark;
        Rule<> not_closing = *terminal<char>([](char c){return c!=']';});
        auto grammar = not_closing >> long_bracket_end;
        auto startpos = c.mark();
        while(!c.ended()) {
            grammar.parse(c);
        }
    });

    lexconv::Rules<Context>::long_bracket_start.setAction([this](decltype(m_context)& c, decltype(m_context)::MatchRange m) {
        int level = m.end() - m.begin() - 2;
        assert(level>=0);
        auto end_mark = ']' >> (level * terminal('=')) >> ']';
        Rule<> long_bracket_end = end_mark;
        Rule<> not_closing = *terminal<char>([](char c){return c!=']';});
        auto grammar = not_closing >> long_bracket_end;
        auto startpos = c.mark();
        while(!c.ended()) {
            grammar.parse(c);
        }
        auto endpos = c.mark() - 2 - level;
        m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
        m_token_buf.info = std::string(startpos, endpos);
    });
    lexconv::Rules<Context>::ops.setAction([this](decltype(m_context)& c, decltype(m_context)::MatchRange m){
        std::string_view result {m.begin(), m.end()};
        assert(result.size() > 0 && result.size() <= 3);
        if(result.size() > 1) {
            auto iter = str2tkid.find(result);
            if(iter != str2tkid.end()) {
                m_token_buf.id = static_cast<Token::TokenIDType>(iter->second);
            }
            else {
                assert(false);
            }
        }
        else {
            m_token_buf.id = int(result[0]);
        }
    });

    lexconv::Rules<Context>::name.setAction([this](decltype(m_context)& c, decltype(m_context)::MatchRange m){
        std::string_view result {m.begin(), m.end()};
        auto iter = str2tkid.find(result);
        if(iter == str2tkid.end()) {
            m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_NAME);
            m_token_buf.info = std::string(result);
        }
        else {
            m_token_buf.id = static_cast<Token::TokenIDType>(iter->second);
        }
    });

    lexconv::Rules<Context>::string_literal.setAction([this](decltype(m_context)& c, decltype(m_context)::MatchRange m) {
        std::string_view result {m.begin()+1, m.end()-1};
        if(m_token_buf.id == -1){
            m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
            m_token_buf.info = std::string(result);
        }
    });

    lexconv::Rules<Context>::numeral.setAction([this](decltype(m_context)& c, decltype(m_context)::MatchRange m) {
        std::string_view result{m.begin(), m.end()};
        int value;
        auto ret = std::from_chars(result.data(), result.data()+result.size(), value);
        if(ret.ptr == result.data()+result.size()){
            m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_INT);
            m_token_buf.info = value;
        }
        else {
            double value;
            auto ret = std::from_chars(result.data(), result.data()+result.size(), value);
            m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_FLT);
            m_token_buf.info = value;
        }
    });
}

template<>
std::optional<Token>Tokenizer<std::span<const typename std::string::value_type>>::next(){
    while(!m_context.ended()) {
        bool ok = lexconv::Rules<Context>::token(m_context);
        if(ok) {
            if(hasToken()){
                return currentToken();
            }
        }
        else{
            m_token_buf.id = -1;
            return currentToken();
        }
    }
    m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_EOS);
    return currentToken();
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

