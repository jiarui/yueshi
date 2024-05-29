#pragma once
#include "peglib.h"
#include <span>
#include <variant>
#include <string_view>
#include <iostream>
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
        };
        

        struct Tokenizer {
        public:
            Tokenizer(const std::string& input);
            std::optional<Token> next();
            void clear() {
                m_token_buf.id = -1;
            }
            bool hasToken() {
                return m_token_buf.id != -1;
            }
            Token currentToken() {
                Token result;
                std::swap(result, m_token_buf);
                return result;
            }
        protected:
            peg::Context<std::span<const std::string::value_type>> m_context;
            Token m_token_buf;
        };
        
    } // namespace lua
}

