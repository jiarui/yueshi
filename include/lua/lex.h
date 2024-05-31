#pragma once
#include <span>
#include <variant>
#include <charconv>
#include "peglib.h"
#include "lex_conv.h"
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
        

        template <peg::InputSourceType InputSource=std::span<const typename std::string::value_type>>
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
            using Context = peg::Context<InputSource>;
            using value_type = typename peg::Context<InputSource>::ValueType;
            using MatchRange = typename peg::Context<InputSource>::MatchRange;
        protected:
            peg::Context<InputSource> m_context;
            Token m_token_buf;
        };

        template<peg::InputSourceType InputSource>
        Tokenizer<InputSource>::Tokenizer(const std::string& input) : m_context(input) {
            lexconv::Rules<Context>::comment_long_bracket_start.setAction([this](Context& c, MatchRange m) {
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

            lexconv::Rules<Context>::long_bracket_start.setAction([this](Context& c, MatchRange m) {
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
            lexconv::Rules<Context>::ops.setAction([this](Context& c, MatchRange m){
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

            lexconv::Rules<Context>::name.setAction([this](Context& c, MatchRange m){
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

            lexconv::Rules<Context>::string_literal.setAction([this](Context& c, MatchRange m) {
                std::string_view result {m.begin()+1, m.end()-1};
                if(m_token_buf.id == -1){
                    m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
                    m_token_buf.info = std::string(result);
                }
            });

            lexconv::Rules<Context>::numeral.setAction([this](Context& c, MatchRange m) {
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

        template<peg::InputSourceType InputSource>
        std::optional<Token> Tokenizer<InputSource>::next(){
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
        
    } // namespace lua
}

