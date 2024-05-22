#include "peglib.h"
#include <span>
#include <variant>
#include <string_view>
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
            using TokenInfo = std::variant<long long, double, std::string_view>;
            Token() = default;
            TokenID id;
            TokenInfo info;
        };
        

        struct Tokenizer {
        public:
            Tokenizer(const std::string& input) : m_context(input) {}
            Token next();
        protected:
            peg::Context<char> m_context;
        };
        
    } // namespace lua
}

