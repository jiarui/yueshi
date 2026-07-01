#include "lua/state.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <utility>

#include "lua/compile.h"
#include "lua/goto_check.h"

namespace ys
{
    namespace lua
    {
        State::State()
            : m_out(&std::cout), m_eval(m_heap, *m_out) {}

        std::optional<AstNode> State::parse_string(const std::string& source,
                                                   std::vector<std::string>& errors)
        {
            auto pr = compile_source(source);
            errors = std::move(pr.errors);
            return std::move(pr.ast);
        }

        ValueVec State::run_string(const std::string& source)
        {
            std::vector<std::string> errors;
            auto ast = parse_string(source, errors);
            if (!ast)
                throw LuaError("syntax error before evaluation", 0);
            check_goto_scopes(*ast, source);
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
                check_goto_scopes(*ast, source);
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
