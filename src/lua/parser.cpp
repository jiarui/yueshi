#include "lua/parser.h"
#include <sstream>

using namespace peg;
using namespace ys::lua;

// ---------------------------------------------------------------------------
// to_display: ADL hook so peglib diagnostics render a token's id (not the
// generic "<token>" placeholder). Defined in the ys::lua namespace so peglib's
// to_display CPO finds it via ADL on Token (ParseError.h:64-70, 134-138).
// We reuse the lexer's operator<< (which prints the token type name).
// ---------------------------------------------------------------------------
namespace ys::lua {
std::string to_display(const Token& t)
{
    std::ostringstream os;
    os << t;
    return os.str();
}

// Build a human-readable "expected ..." suffix from a Diagnostic's expected
// set, without requiring a SourceMap (the Parser only has tokens).
static std::string diagnostic_text(const peg::Diagnostic& d)
{
    std::ostringstream os;
    const auto& exp = d.expected();
    if (exp.empty()) {
        os << "unexpected input";
    } else {
        os << "expected ";
        bool first = true;
        for (const auto& item : exp) {
            if (!first) os << " or ";
            first = false;
            os << item.text;
        }
    }
    return os.str();
}
} // namespace ys::lua

// ---------------------------------------------------------------------------
// Parser implementation
// ---------------------------------------------------------------------------

Parser::Parser(std::vector<Token> tokens)
    : m_tokens(std::move(tokens))
    , m_context(m_tokens)
    , m_grammar(parserconv::make_grammar())
{
}

std::optional<AstNode> Parser::parse()
{
    // parse_ast is the only entry point that runs typed actions (the post-
    // parse fold) and returns the folded AST. parse_tree returns pure
    // structure with no values and never fires actions. The start rule is
    // "chunk" (the grammar's set_start target); see parser_conv.h.
    auto ast = m_grammar.parse_ast("chunk", m_context);
    m_consumed = m_context.mark();
#ifdef YS_PARSER_DEBUG
    std::cerr << "[parser] parse_ast: ast=" << (ast ? "has_value" : "nullopt")
              << " consumed=" << m_consumed << " ntokens=" << m_tokens.size() << "\n";
#endif
    if (ast && m_consumed + 1 < m_tokens.size()) {
        // Did not reach the EOS sentinel: the block did not cover the input.
        return std::nullopt;
    }
    return ast;
}

std::vector<std::string> Parser::take_errors()
{
    std::vector<std::string> out;
    auto emit = [this](std::size_t tok_idx, const std::string& text) {
        std::size_t byte = (tok_idx < m_tokens.size()) ? m_tokens[tok_idx].start : 0;
        return "token " + std::to_string(tok_idx) +
               " (byte " + std::to_string(byte) + "): " + text;
    };
    // Furthest-failure channel (single). Suppress failures that did not reach
    // beyond what the successful parse consumed — those are normal PEG
    // backtracking at a block/statement boundary (the closing keyword or EOS).
    if (auto d = m_context.take_error()) {
        if (d->position() > m_consumed) {
            out.push_back(emit(d->position(), diagnostic_text(*d)));
        }
    }
    // Recovery-point channel (multi).
    for (auto& d : m_context.take_diagnostics()) {
        out.push_back(emit(d.position(), diagnostic_text(d)));
    }
    return out;
}

