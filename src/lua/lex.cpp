#include "lua/lex.h"
#include <type_traits>
using namespace peg;
using namespace ys::lua;

using namespace peg;

namespace lexconv
{
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
    Rule<std::string::value_type> token = numeral | name | string_literal | comment | WS;

    
    
} // namespace lexconv
#define STR_ELEMENT(p) {p, #p}

const static std::vector<std::tuple<TokenID, std::string_view>> tk_type_str = {
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

std::ostream& operator<<(std::ostream& s, const Token& t) {
    auto id = static_cast<typename std::underlying_type<TokenID>::type>(t.id);
    auto index = id - UCHAR_MAX;
    s<<"Token Type"<< id << ':' << std::get<1>(tk_type_str[index])<<std::endl;
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
}

Token Tokenizer::next() {
    if(!m_context.ended()){
        bool ok = lexconv::token(m_context);
        if(ok) {
            return m_token_buf;
        }
    }
    return {};
}




