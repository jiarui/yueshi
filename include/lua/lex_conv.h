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
    // Lua identifiers are ASCII-only [A-Za-z0-9_]. Cast to unsigned char before
    // any classification: std::isalpha/isalnum on a negative (signed char)
    // value is UB, and they are locale-dependent anyway. Use explicit ASCII
    // ranges so a UTF-8 source lexes deterministically.
    auto is_name_start = [](char c) {
        auto u = static_cast<unsigned char>(c);
        return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_';
    };
    auto is_name_cont = [](char c) {
        auto u = static_cast<unsigned char>(c);
        return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
               (u >= '0' && u <= '9') || u == '_';
    };
    auto name_start = terminal<char>(is_name_start);
    auto name_cont = terminal<char>(is_name_cont);
    auto linebreak = terminalSeq<char>("\r\n") | terminal('\n');
    auto digit = terminal('0', '9');
    auto xdigit = terminal<char>([](char c) {
        auto u = static_cast<unsigned char>(c);
        return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') ||
               (u >= 'A' && u <= 'F');
    });

    // Numeric literals NEVER carry a leading sign — '-'/'+' before a numeral
    // are unary/binary operators, not part of the literal (so `3-2` lexes as
    // three tokens, and `-3` as two). Only the EXPONENT may have a sign
    // (1e-4, 0x1p-4), handled by `signed_decimal` below.
    auto fractional = (*digit >> '.' >> +digit) | (+digit >> '.' >> *digit);
    auto decimal = +digit;
    auto signed_decimal = -(terminal('+') | '-') >> decimal;
    auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >>
                      -('.' >> +xdigit) >> -((terminal('p') | 'P') >> +signed_decimal);

    auto common_escape_code =
        terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' |
        (terminal('\\') >> '\\' >> 'n') | ('z' >> WS) | (3 * digit) |
        (2 * xdigit) | (terminal('u') >> '{' >> *xdigit >> '}');
    auto single_escape_code = terminal('\\') >> (common_escape_code | '\'');
    // Inside a double-quoted string the escapable quote is ", not '. (The
    // previous code allowed \' here, which made "a\"b" un-lexable.)
    auto double_escape_code = terminal('\\') >> (common_escape_code | '"');
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
    // Short comment body runs to end-of-line; the terminating newline is
    // OPTIONAL so a `-- comment` at EOF (no trailing newline) still lexes as a
    // comment rather than falling through to '-' '-' name.
    g["comment"] = terminal('-') >> '-' >>
                   (g["comment_long_bracket_start"] | (*not_linebreak >> -linebreak));
    g["numeral"] = hexdecimal |
                   ((fractional | decimal) >> -((terminal('e') | 'E') >> -(signed_decimal)));
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
