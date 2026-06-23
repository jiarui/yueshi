#pragma once
#include <cassert>
#include <charconv>
#include <climits> // UCHAR_MAX (used for the TokenID base offset below)
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "peglib.h"
#include "lua/lex_conv.h"

namespace ys
{
    namespace lua
    {
        enum class TokenID : std::intmax_t {
            TK_AND = UCHAR_MAX + 1, TK_BREAK,
            TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
            TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
            TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
            /* other terminal symbols */
            TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
            TK_SHL, TK_SHR,
            TK_DBCOLON, TK_EOS,
            TK_FLT, TK_INT, TK_NAME, TK_STRING
        };

        struct Token{
            using TokenInfo = std::variant<long long, double, std::string>;
            using TokenIDType = std::underlying_type_t<TokenID>;
            Token() = default;
            TokenIDType id{-1};
            TokenInfo info;
            // Source range as byte offsets into the original input. start is
            // inclusive, end is exclusive (length == end - start). Used for
            // diagnostics and (later) parser source spans.
            std::size_t start{0};
            std::size_t end{0};
            friend std::ostream& operator<<(std::ostream&, const Token&);
            // const so Token satisfies std::equality_comparable (peglib's
            // PegValue concept), enabling token-level parsing via
            // terminal(Token) / terminalSeq(vector<Token>).
            bool operator==(const Token& rhs) const;
        };

        // Declared in the namespace (not just as a friend inside Token) so the
        // qualified definition in lex.cpp resolves cleanly.
        std::ostream& operator<<(std::ostream& s, const Token& t);

        extern const std::map<std::string_view, TokenID> str2tkid;

        class Tokenizer {
        public:
            // peglib's Context is now templated on the character type (char),
            // not the input range. MatchRange was removed; semantic actions
            // read matched text via node offsets + Context::substr.
            using Context = peg::Context<char>;

            explicit Tokenizer(std::string input);

            // Eagerly tokenize the entire input. Returns the full token stream
            // with a trailing TK_EOS sentinel. On a lexing error the last
            // element before EOS is a sentinel Token with id == -1 and the
            // caller can retrieve a diagnostic via take_error().
            //
            // This is the primary interface for the parser: peglib's token-
            // level Context<Token> needs a contiguous token sequence, which a
            // pull-style next() cannot provide. The pull-style API was removed
            // in favour of this single eager entry point.
            std::vector<Token> tokenize();

            // Pop the furthest lexing failure (if any) formatted as
            // "file:line:col: error: expected ...". Returns std::nullopt when
            // the last tokenize() run had no error. The Tokenizer keeps the
            // input alive so a peg::SourceMap can be built on demand.
            std::optional<std::string> take_error();
        protected:
            peg::Grammar<> m_grammar;
            // m_input MUST be declared before m_context: members are initialized
            // in declaration order, and Context holds a non-owning SpanSource
            // pointing into this string. If m_context were initialized first it
            // would capture the constructor's by-value parameter, which is
            // destroyed at the end of the constructor — a dangling pointer.
            std::string m_input;
            Context m_context;
            // Per-token scratch written by the semantic actions during a
            // single grammar.parse("token") call; tokenize() swaps it out
            // after each successful scan. Reset to id == -1 before each parse.
            Token m_token_buf;

            // Scan the remaining input for `marker`, advancing the context past
            // it on success. Returns true if the marker was found and consumed.
            static bool consume_until_close(Context& c, const std::string& marker);
        };

    } // namespace lua
}