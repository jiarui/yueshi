#pragma once
#include "peglib.h"

namespace ys::lua::lexconv {

// Build the Lua 5.4 lexer grammar. Each Tokenizer instance owns a fresh
// Grammar so its semantic actions can capture Tokenizer state independently.
inline peg::Grammar<> make_grammar()
{
    using namespace peg;

    Grammar<> g;

    auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
    auto not_linebreak = terminal<char>([](char c) { return c != '\n'; });
    auto name_start = terminal<char>([](char c) { return std::isalpha(c) || c == '_'; });
    auto name_cont = terminal<char>([](char c) { return std::isalnum(c) || c == '_'; });
    auto linebreak = terminalSeq<char>("\r\n") | terminal('\n');
    auto digit = terminal('0', '9');
    auto xdigit = terminal<char>([](char c) { return std::isxdigit(c); });
    auto pos_or_neg = (terminal('+') | '-');
    auto fractional =
        -pos_or_neg >> ((*digit >> '.' >> +digit) | (+digit >> '.' >> *digit));
    auto decimal = -pos_or_neg >> +digit;
    auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >>
                      -('.' >> +xdigit) >> -((terminal('p') | 'P') >> +decimal);

    auto common_escape_code =
        terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' |
        (terminal('\\') >> '\\' >> 'n') | ('z' >> WS) | (3 * digit) |
        (2 * xdigit) | (terminal('u') >> '{' >> *xdigit >> '}');
    auto single_escape_code = terminal('\\') >> (common_escape_code | '\'');
    auto double_escape_code = terminal('\\') >> (common_escape_code | '\'');
    auto single_no_escape_code = terminal<char>([](char c) { return c != '\''; });
    auto double_no_escape_code = terminal<char>([](char c) { return c != '"'; });

    g["name"] = name_start >> *name_cont;
    g["long_bracket_start"] = '[' >> *terminal('=') >> '[';
    g["comment_long_bracket_start"] = '[' >> *terminal('=') >> '[';
    auto string_single_quote =
        '\'' >> *(single_escape_code | single_no_escape_code) >> '\'';
    auto string_double_quote =
        '"' >> *(double_escape_code | double_no_escape_code) >> '"';
    g["string_literal"] =
        string_single_quote | string_double_quote | g["long_bracket_start"];
    g["comment"] = terminal('-') >> '-' >>
                   (g["comment_long_bracket_start"] | (*not_linebreak >> linebreak));
    g["numeral"] = hexdecimal |
                   ((fractional | decimal) >> -((terminal('e') | 'E') >> -(decimal)));
    g["ops"] = terminalSeq("...") | terminalSeq("..") | terminalSeq("<<") |
               terminalSeq(">>") | terminalSeq("//") | terminalSeq("==") |
               terminalSeq("~=") | terminalSeq("<=") | terminalSeq(">=") |
               terminalSeq("::") | '+' | '-' | '*' | '/' | '%' | '^' | '#' |
               '&' | '~' | '|' | '<' | '>' | '=' | '(' | ')' | '{' | '}' |
               '[' | ']' | ';' | ':' | ',' | '.';

    auto cut_ = cut<>();
    g["token"] = g["comment"] | (WS >> cut_) | (g["numeral"] >> cut_) |
                 (g["name"] >> cut_) | (g["string_literal"] >> cut_) |
                 g["ops"] | linebreak;

    return g;
}

} // namespace ys::lua::lexconv
