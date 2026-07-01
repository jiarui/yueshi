#include "lua/compile.h"

#include <utility>

#include "lua/lex.h"
#include "lua/parser.h"

namespace ys
{
    namespace lua
    {
        ParseResult compile_source(std::string source)
        {
            ParseResult result;

            Tokenizer tok{std::move(source)};
            std::vector<Token> tokens = tok.tokenize();

            // A lex error is signalled by a sentinel token with id == -1
            // sitting just before the EOS token. Only then is the furthest-
            // failure diagnostic meaningful (peglib's take_error can return a
            // spurious "expected ..." from normal end-of-stream backtracking on
            // a CLEAN lex, which must not be reported as an error).
            bool lex_failed = false;
            for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
                if (tokens[i].id == -1) lex_failed = true;
            if (lex_failed)
                if (auto le = tok.take_error())
                    result.errors.push_back(*le);

            Parser p{std::move(tokens)};
            result.ast = p.parse();
            for (auto& e : p.take_errors())
                result.errors.push_back(std::move(e));

            if (lex_failed) result.ast = std::nullopt;
            return result;
        }
    } // namespace lua
} // namespace ys
