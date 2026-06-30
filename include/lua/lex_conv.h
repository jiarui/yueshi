#pragma once
#include "peglib.h"

namespace ys::lua::lexconv {

// Build the Lua 5.4 lexer grammar. Each Tokenizer instance owns a fresh
// Grammar so its semantic actions can capture Tokenizer state independently.
//
// peglib API note: since the b4c7ed7 bump, terminal/terminalSeq/cut factories
// are Grammar MEMBER factories (g.terminal(...), g.terminalSeq(...), g.cut()),
// not free functions. The free factories hardcoded Context<elem, monostate>
// and so could not produce expressions for a Grammar with a custom NodeType;
// the member factories close over the Grammar's own Context. Bare character
// literals inside >> / | / * / + still work via the value-literal overloads
// (operator>> / operator| build a TerminalExpr from the operand's Context), so
// only standalone factory CALLS switch to g.xxx; chars embedded in a sequence
// or choice are unchanged.
inline peg::Grammar<> make_grammar()
{
    using namespace peg;

    Grammar<> g;

    auto WS = +g.terminal(std::set({' ', '\f', '\t', '\v'}));
    // A "not linebreak" char excludes all three newline forms: \n (Unix),
    // \r\n (Windows), and bare \r (old Mac). Excluding \r here keeps a comment
    // body from running past a CR-only line ending.
    auto not_linebreak = g.terminal([](char c) { return c != '\n' && c != '\r'; });
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
    auto name_start = g.terminal(is_name_start);
    auto name_cont = g.terminal(is_name_cont);
    // Newline: \r\n (Windows), \n (Unix), or bare \r (old Mac). CRLF must be
    // tried FIRST — PEG ordered choice, else the lone-\r branch would eat the
    // CR half of a CRLF before the LF could be matched.
    auto linebreak = g.terminalSeq("\r\n") | g.terminal('\n') | g.terminal('\r');
    auto digit = g.terminal('0', '9');
    auto xdigit = g.terminal([](char c) {
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
    auto signed_decimal = -(g.terminal('+') | '-') >> decimal;
    // Hex numeral (Lua 5.4 §3.1). An integer part OR a fraction part must be
    // present, but EITHER may be empty: 0x.0p-3 (no integer part) is just as
    // valid as 0x1p0 or 0xFF. The previous form (+xdigit >> -('.' >> +xdigit))
    // forced the integer part to be non-empty, so 0x.0p-3 fell through and
    // mis-lexed. Hex float needs a p-exponent; a bare integer takes no part.
    auto hex_int_or_frac =
        (+xdigit >> -('.' >> *xdigit)) | ('.' >> +xdigit);
    auto hexdecimal = g.terminal('0') >> (g.terminal('x') | 'X') >>
                      hex_int_or_frac >>
                      -((g.terminal('p') | 'P') >> +signed_decimal);

    auto common_escape_code =
        g.terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' |
        // Escaped backslash (\\). This was missing entirely; the previous
        // (terminal('\\') >> '\\' >> 'n') tried to cover it but matched the
        // literal three characters \\n instead and never accepted a lone \\.
        g.terminal('\\') |
        // Line continuation: backslash followed by a real newline (LF or
        // CRLF). The result is the empty string; the grammar just needs to
        // consume it so the newline is not treated as an unescaped line break
        // (which short strings reject).
        linebreak |
        // \z skips all following whitespace INCLUDING newlines (Lua 5.4 §3.1).
        // The lexer's WS excludes newlines (inter-token whitespace never spans
        // lines), so \z uses its own newline-inclusive whitespace class.
        ('z' >> +g.terminal(std::set({' ', '\f', '\t', '\v', '\n', '\r'}))) |
        // Decimal escape \ddd: 1 to 3 digits (Lua 5.4 §3.1). The previous
        // "(3 * digit)" required EXACTLY 3 digits, so '\0' or '\65' (1-2
        // digits) failed lexing. Greedy 1-then-optional-2 matches the same
        // set as "up to 3" because PEG always takes the optional digits when
        // available, capping at 3 total.
        (digit >> -digit >> -digit) |
        (2 * xdigit) | (g.terminal('u') >> '{' >> *xdigit >> '}');
    auto single_escape_code = g.terminal('\\') >> (common_escape_code | '\'');
    // Inside a double-quoted string the escapable quote is ", not '. (The
    // previous code allowed \' here, which made "a\"b" un-lexable.)
    auto double_escape_code = g.terminal('\\') >> (common_escape_code | '"');
    // A short string must NOT contain a raw newline (Lua 5.4 §3.1: a newline
    // may only appear escaped). The previous predicate accepted any char but
    // the quote, so an unterminated string silently swallowed newlines and
    // grabbed text far past its real end — the official corpus exposed this
    // (e.g. a missing close quote let the scan run on to the next '"' on a
    // later line). Rejecting \n and \r here makes such input a lex error at
    // the right place instead.
    auto single_no_escape_code =
        g.terminal([](char c) { return c != '\'' && c != '\n' && c != '\r'; });
    auto double_no_escape_code =
        g.terminal([](char c) { return c != '"' && c != '\n' && c != '\r'; });

    g["name"] = name_start >> *name_cont;

    // Long-bracket matcher (match-time primitive). The opening bracket level N
    // (number of '=' between the two '[') determines the closing bracket: "]"
    // + N×"=" + "]". Pure PEG cannot express this dynamic level, so a matcher
    // fn handles the entire construct [=*[ ... ]=*] and reports the consumed
    // span. MatcherExpr owns the position advance; the fn only reads the input.
    auto lua_long_bracket = [](auto& c, Span) -> std::optional<Span> {
        std::size_t pos = c.mark();
        if (pos + 2 > c.input_size() || c.at(pos) != '[') return std::nullopt;
        std::size_t level = 0, i = pos + 1;
        while (i < c.input_size() && c.at(i) == '=') { ++level; ++i; }
        if (i >= c.input_size() || c.at(i) != '[') return std::nullopt;
        ++i;
        // Skip immediately-following newline (Lua convention).
        if (i < c.input_size()) {
            if (c.at(i) == '\r') { ++i; if (i < c.input_size() && c.at(i) == '\n') ++i; }
            else if (c.at(i) == '\n') ++i;
        }
        std::size_t scan = i;
        while (scan + level + 2 <= c.input_size()) {
            if (c.at(scan) == ']' && c.at(scan + level + 1) == ']') {
                bool ok = true;
                for (std::size_t j = 0; j < level; ++j)
                    if (c.at(scan + 1 + j) != '=') { ok = false; break; }
                if (ok) return Span{pos, scan + level + 2};
            }
            ++scan;
        }
        return std::nullopt;
    };
    g["long_bracket_start"] = g.matcher(lua_long_bracket);
    g["comment_long_bracket_start"] = g.matcher(lua_long_bracket);
    auto string_single_quote =
        '\'' >> *(single_escape_code | single_no_escape_code) >> '\'';
    auto string_double_quote =
        '"' >> *(double_escape_code | double_no_escape_code) >> '"';
    g["string_literal"] =
        string_single_quote | string_double_quote | g["long_bracket_start"];
    // Short comment body runs to end-of-line; the terminating newline is
    // OPTIONAL so a `-- comment` at EOF (no trailing newline) still lexes as a
    // comment rather than falling through to '-' '-' name.
    g["comment"] = g.terminal('-') >> '-' >>
                   (g["comment_long_bracket_start"] | (*not_linebreak >> -linebreak));
    g["numeral"] = hexdecimal |
                   ((fractional | decimal) >> -((g.terminal('e') | 'E') >> -(signed_decimal)));
    g["ops"] = g.terminalSeq("...") | g.terminalSeq("..") | g.terminalSeq("<<") |
               g.terminalSeq(">>") | g.terminalSeq("//") | g.terminalSeq("==") |
               g.terminalSeq("~=") | g.terminalSeq("<=") | g.terminalSeq(">=") |
               g.terminalSeq("::") | '+' | '-' | '*' | '/' | '%' | '^' | '#' |
               '&' | '~' | '|' | '<' | '>' | '=' | '(' | ')' | '{' | '}' |
               '[' | ']' | ';' | ':' | ',' | '.';

    auto cut_ = g.cut();
    g["token"] = g["comment"] | (WS >> cut_) | (g["numeral"] >> cut_) |
                 (g["name"] >> cut_) | (g["string_literal"] >> cut_) |
                 g["ops"] | linebreak;

    return g;
}

} // namespace ys::lua::lexconv
