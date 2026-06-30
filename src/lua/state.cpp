#include "lua/state.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <utility>

#include "lua/lex.h"
#include "lua/parser.h"

namespace ys
{
    namespace lua
    {
        State::State()
            : m_out(&std::cout), m_eval(m_heap, *m_out) {}

        std::optional<AstNode> State::parse_string(const std::string& source,
                                                   std::vector<std::string>& errors)
        {
            Tokenizer tok{source};
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
                if (auto le = tok.take_error()) errors.push_back(*le);

            Parser p{std::move(tokens)};
            std::optional<AstNode> ast = p.parse();
            for (auto& e : p.take_errors()) errors.push_back(std::move(e));

            if (lex_failed) return std::nullopt;
            return ast;
        }

        ValueVec State::run_string(const std::string& source)
        {
            std::vector<std::string> errors;
            auto ast = parse_string(source, errors);
            if (!ast)
                throw LuaError("syntax error before evaluation", 0);
            return m_eval.run(std::move(*ast));
        }

        // Read a whole file into a string.
        static std::string slurp(const std::string& path)
        {
            std::ifstream is(path, std::ios::binary);
            return std::string{std::istreambuf_iterator<char>{is}, {}};
        }

        bool State::run_file(const std::string& path)
        {
            std::string source = slurp(path);
            std::vector<std::string> errors;
            auto ast = parse_string(source, errors);

            if (!errors.empty()) {
                for (const auto& e : errors) *m_out << e << "\n";
                return false;
            }
            if (!ast) {
                *m_out << path << ": parse failed\n";
                return false;
            }
            try {
                m_eval.run(std::move(*ast));
                return true;
            }
            catch (const LuaError& e) {
                *m_out << "lua: " << e.what() << "\n";
                return false;
            }
        }

    } // namespace lua
} // namespace ys
