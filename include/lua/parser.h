#pragma once

// yueshi Lua 5.4 parser: token stream → AST.
//
// Architecture: peglib token-level parsing. A Grammar<Token, AstNode> drives a
// Context<Token> over the token vector produced by Tokenizer::tokenize().
// Semantic actions build the variant AST (ast.h) from the parse tree.
//
// Member-order constraint (HARD, mirrors the Phase 1 Tokenizer fix): m_tokens
// is declared BEFORE m_context. Context holds a non-owning SpanSource pointing
// into the token vector; if m_context were initialized first it would capture
// the constructor's by-value parameter (destroyed at ctor exit) — a dangling
// pointer. The Phase 1 Tokenizer hit this exact bug (ASan stack-use-after-
// scope); this file replicates the fix.
//
// See parser_conv.h for the grammar rules and ast.h for the node types.

#include <optional>
#include <string>
#include <vector>

#include "peglib.h"
#include "lua/ast.h"
#include "lua/lex.h"  // Token, TokenID
#include "lua/parser_conv.h"

namespace ys
{
    namespace lua
    {
        class Parser {
        public:
            // Parse the token stream produced by Tokenizer::tokenize(). The
            // Parser keeps the token vector alive (m_tokens) for the lifetime
            // of the Context that indexes into it.
            explicit Parser(std::vector<Token> tokens);

            // Parse and return the root Chunk AST node, or std::nullopt if the
            // chunk rule did not match at all (a top-level failure). Partial
            // failures are reported via diagnostics; the returned AST reflects
            // whatever was recoverable.
            std::optional<AstNode> parse();

            // Drain accumulated diagnostics. Each returned string is one
            // diagnostic, formatted as:
            //   "token N (byte <off>): error: ..."
            // where N is the token index and <off> is the token's source byte
            // offset (Token::start). The Parser only has tokens, not the
            // original source text, so it cannot resolve line:col directly —
            // the caller (e.g. State, which holds the source file) can map
            // <off> via a peg::SourceMap for a human-readable location.
            //
            // Combines peglib's furthest-failure (take_error) and recovery-
            // point (take_diagnostics) channels. Returns an empty vector if
            // parse() produced no errors.
            std::vector<std::string> take_errors();

        private:
            // ① FIRST: owns the token storage the Context indexes into.
            std::vector<Token> m_tokens;
            // ② SECOND: non-owning SpanSource pointing at m_tokens. NodeType
            // must match the Grammar's so actions return AstNode.
            peg::Context<Token, AstNode> m_context;
            // Grammar is independent of the token data; order is free.
            peg::Grammar<Token, AstNode> m_grammar;
            // How far the last parse() consumed (a token index). Used by
            // take_errors() to suppress normal PEG end-of-stream backtracking
            // from the furthest-failure channel.
            std::size_t m_consumed{0};
        };

    } // namespace lua
} // namespace ys
