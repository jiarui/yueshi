#include "lua/lex.h"
#include <type_traits>
using namespace peg;
using namespace ys::lua;

using namespace peg;

namespace lexconv
{
    Rule<std::string::value_type> ops = 
        terminalSeq("...") |
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
    auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
    auto not_linebreak = terminal<char>([](char c){return c!='\n';});
    auto name_start = terminal<char>([](char c){return std::isalpha(c) || c == '_';});
    auto name_cont = terminal<char>([](char c){return std::isalnum(c) || c=='_';});
    Rule<std::string::value_type> name =  name_start >> *name_cont;
    auto linebreak = terminalSeq<char>("\r\n") | terminal('\n');
    auto digit = terminal('0', '9');
    auto xdigit = terminal<char>([](char c){return std::isxdigit(c);});
    auto pos_or_neg = (terminal('+') | '-');
    auto fractional = -pos_or_neg >> ((*digit >> '.' >> +digit) | (+digit >> '.' >> *digit));
    auto decimal = -pos_or_neg >> +digit;
    auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >> -('.' >> +xdigit) >> -((terminal('p') | 'P') >> +decimal);
    auto expotent = -pos_or_neg >> +digit;
    Rule<std::string::value_type> numeral = hexdecimal | ((fractional | decimal) >> (terminal('e') | 'E') >> -(decimal));

    auto common_escape_code = terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' | (terminal('\\') >>'\\'>>'n')| ('z' >> WS) | (3 * digit) | (2 * xdigit) | (terminal('u') >> '{' >> *xdigit >> '}') ;
    Rule<std::string::value_type> single_escape_code = terminal('\\') >> ( common_escape_code | '\'' );
    Rule<std::string::value_type> double_escape_code = terminal('\\') >> ( common_escape_code | '\'' );
    auto single_no_escape_code = terminal<char>([](char c){return c != '\'';});
    auto double_no_escape_code = terminal<char>([](char c){return c != '"';});
    auto string_single_quote = '\'' >> *(single_escape_code | single_no_escape_code) >> '\'';
    auto string_double_quote = '"' >> *(double_escape_code | double_no_escape_code) >> '"';
    Rule<std::string::value_type> long_bracket_start = '[' >> *terminal('=') >> '[';
    Rule<std::string::value_type> string_literal = string_single_quote | string_double_quote | long_bracket_start;
    
    Rule<std::string::value_type> comment = terminal('-') >> '-' >> (long_bracket_start | (*not_linebreak >> linebreak));
    Rule<std::string::value_type> token = numeral | name | ops | string_literal | comment | WS | linebreak;

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

Tokenizer::Tokenizer(const std::string& input) : m_context(input) {
    lexconv::long_bracket_start.setAction([](Context<char>& c, Context<char>::MatchRange m) {
        int level = m.end() - m.begin() - 2;
        assert(level>0);
        Rule<char> long_bracket_end = ']' >> (level * terminal('=')) >> ']';
        Rule<char> not_closing = *terminal<char>([](char c){return c!=']';});
        auto grammar = not_closing >> long_bracket_end;
        while(!c.ended()) {
            grammar.parse(c);
        }
    });
    lexconv::ops.setAction([this](Context<char>& c, Context<char>::MatchRange m){
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

    lexconv::name.setAction([this](Context<char>& c, Context<char>::MatchRange m){
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

    lexconv::string_literal.setAction([this](Context<char>& c, Context<char>::MatchRange m) {
        std::string_view result {m.begin()+1, m.end()-1};
        m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
        m_token_buf.info = std::string(result);
    });
}

std::optional<Token> Tokenizer::next() {
    while(!m_context.ended()) {
        bool ok = lexconv::token(m_context);
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

