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
            friend std::ostream& operator<<(std::ostream&, const Token&);
            bool operator==(const Token& rhs);
        };

        extern const std::map<std::string_view, TokenID> str2tkid;

        class Tokenizer {
        public:
            // peglib's Context is now templated on the character type (char),
            // not the input range. MatchRange was removed; semantic actions
            // read matched text via node offsets + Context::substr.
            using Context = peg::Context<char>;

            Tokenizer(const std::string& input);
            std::optional<Token> next();
            void clear() {
                m_token_buf.id = -1;
            }
            bool hasToken() const {
                return m_token_buf.id != -1;
            }
            Token currentToken() {
                Token result;
                std::swap(result, m_token_buf);
                return result;
            }
        protected:
            peg::Grammar<> m_grammar;
            Context m_context;
            Token m_token_buf;

            // Scan the remaining input for `marker`, advancing the context past
            // it on success. Returns true if the marker was found and consumed.
            static bool consume_until_close(Context& c, const std::string& marker);
        };

    } // namespace lua
}
