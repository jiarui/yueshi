#pragma once
#include "peglib.h"
namespace ys{
namespace lua{
using peg::Rule;
using peg::terminal;
using peg::terminalSeq;
using peg::cut;
namespace lexconv
{
    template<typename Context>
    struct Rules {
        using value_type = typename Context::ValueType;
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
}
}