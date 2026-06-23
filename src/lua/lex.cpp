#include "lua/lex.h"
#include <type_traits>
#include <string>
#include <string_view>
#include <iostream>
using namespace peg;
using namespace ys::lua;

// ---------------------------------------------------------------------------
// Tokenizer implementation
// ---------------------------------------------------------------------------

bool Tokenizer::consume_until_close(Context& c, const std::string& marker)
{
    // Positions are now byte offsets into the input source (get_input() was
    // removed in favour of the type-erased InputSource). Scan forward from
    // the current offset until the closing marker is matched, consuming it.
    while (!c.ended()) {
        auto save = c.mark();
        bool match = true;
        for (char ec : marker) {
            if (c.ended() || c.current() != ec) {
                match = false;
                break;
            }
            c.next();
        }
        if (match) {
            return true;
        }
        c.reset(save);
        c.next();
    }
    return false;
}

Tokenizer::Tokenizer(const std::string& input)
    : m_grammar(lexconv::make_grammar())
    , m_context(input)
{
    // Long-bracket comment body: consume up to the matching `]=...]` and emit
    // no token. (Opening `[==[` already consumed by the rule.)
    m_grammar["comment_long_bracket_start"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            int level = static_cast<int>(node->end_offset - node->start_offset) - 2;
            assert(level >= 0);
            std::string end_marker = "]" + std::string(level, '=') + "]";
            consume_until_close(c, end_marker);
            return {};
        });

    // Long-bracket string body: consume up to the matching `]=...]`, skipping
    // an immediately following newline per Lua convention.
    m_grammar["long_bracket_start"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            int level = static_cast<int>(node->end_offset - node->start_offset) - 2;
            assert(level >= 0);
            std::string end_marker = "]" + std::string(level, '=') + "]";
            auto body_start = c.mark();
            if (!c.ended()) {
                if (c.current() == '\r') {
                    c.next();
                    if (!c.ended() && c.current() == '\n') c.next();
                    body_start = c.mark();
                } else if (c.current() == '\n') {
                    c.next();
                    body_start = c.mark();
                }
            }
            if (consume_until_close(c, end_marker)) {
                auto body_end = c.mark() - end_marker.size();
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
                m_token_buf.info = c.substr(body_start, body_end - body_start);
            }
            return {};
        });

    m_grammar["ops"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            // Owned copy — keep alive for the lookup; do not bind to string_view
            // (substr returns by value, so a string_view over it would dangle).
            auto result = c.substr(node->start_offset, node->end_offset - node->start_offset);
            assert(result.size() > 0 && result.size() <= 3);
            if (result.size() > 1) {
                auto iter = str2tkid.find(result);
                if (iter != str2tkid.end()) {
                    m_token_buf.id = static_cast<Token::TokenIDType>(iter->second);
                } else {
                    assert(false);
                }
            } else {
                m_token_buf.id = int(result[0]);
            }
            return {};
        });

    m_grammar["name"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            auto result = c.substr(node->start_offset, node->end_offset - node->start_offset);
            auto iter = str2tkid.find(result);
            if (iter == str2tkid.end()) {
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_NAME);
                m_token_buf.info = result;
            } else {
                m_token_buf.id = static_cast<Token::TokenIDType>(iter->second);
            }
            return {};
        });

    m_grammar["string_literal"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            // long_bracket_start already produced the token; only handle the
            // single/double-quoted forms (strip the surrounding quote).
            if (m_token_buf.id == -1) {
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
                m_token_buf.info = c.substr(node->start_offset + 1,
                                            (node->end_offset - node->start_offset) - 2);
            }
            return {};
        });

    m_grammar["numeral"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            auto result = c.substr(node->start_offset, node->end_offset - node->start_offset);
            int value;
            auto ret = std::from_chars(result.data(), result.data() + result.size(), value);
            if (ret.ptr == result.data() + result.size()) {
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_INT);
                m_token_buf.info = value;
            } else {
                double dval;
                std::from_chars(result.data(), result.data() + result.size(), dval);
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_FLT);
                m_token_buf.info = dval;
            }
            return {};
        });
}

std::optional<Token> Tokenizer::next()
{
    while (!m_context.ended()) {
        bool ok = m_grammar.parse("token", m_context);
        if (ok) {
            if (hasToken()) {
                return currentToken();
            }
            // else: whitespace/comment matched; continue scanning.
        } else {
            m_token_buf.id = -1;
            return currentToken();
        }
    }
    m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_EOS);
    return currentToken();
}

#define STR_ELEMENT(p) #p

const static std::vector<const char *> tk_type_str = {
    STR_ELEMENT(TokenID::TK_AND),
    STR_ELEMENT(TokenID::TK_BREAK),
    STR_ELEMENT(TokenID::TK_ELSE),
    STR_ELEMENT(TokenID::TK_ELSEIF),
    STR_ELEMENT(TokenID::TK_FALSE),
    STR_ELEMENT(TokenID::TK_FOR),
    STR_ELEMENT(TokenID::TK_FUNCTION),
    STR_ELEMENT(TokenID::TK_IN),
    STR_ELEMENT(TokenID::TK_LOCAL),
    STR_ELEMENT(TokenID::TK_NIL),
    STR_ELEMENT(TokenID::TK_NOT),
    STR_ELEMENT(TokenID::TK_OR),
    STR_ELEMENT(TokenID::TK_REPEAT),
    STR_ELEMENT(TokenID::TK_RETURN),
    STR_ELEMENT(TokenID::TK_THEN),
    STR_ELEMENT(TokenID::TK_TRUE),
    STR_ELEMENT(TokenID::TK_UNTIL),
    STR_ELEMENT(TokenID::TK_WHILE),
    STR_ELEMENT(TokenID::TK_IDIV),
    STR_ELEMENT(TokenID::TK_CONCAT),
    STR_ELEMENT(TokenID::TK_DOTS),
    STR_ELEMENT(TokenID::TK_EQ),
    STR_ELEMENT(TokenID::TK_GE),
    STR_ELEMENT(TokenID::TK_LE),
    STR_ELEMENT(TokenID::TK_NE),
    STR_ELEMENT(TokenID::TK_SHL),
    STR_ELEMENT(TokenID::TK_SHR),
    STR_ELEMENT(TokenID::TK_DBCOLON),
    STR_ELEMENT(TokenID::TK_EOS),
    STR_ELEMENT(TokenID::TK_FLT),
    STR_ELEMENT(TokenID::TK_INT),
    STR_ELEMENT(TokenID::TK_NAME),
    STR_ELEMENT(TokenID::TK_STRING)
};

#undef STR_ELEMENT

const std::map<std::string_view, TokenID> ys::lua::str2tkid = {
    {"and", TokenID::TK_AND},
    {"break", TokenID::TK_BREAK},
    {"else", TokenID::TK_ELSE},
    {"elseif", TokenID::TK_ELSEIF},
    {"end", TokenID::TK_END},
    {"false", TokenID::TK_FALSE},
    {"for", TokenID::TK_FOR},
    {"function", TokenID::TK_FUNCTION},
    {"goto", TokenID::TK_GOTO},
    {"if", TokenID::TK_IF},
    {"in", TokenID::TK_IN},
    {"local", TokenID::TK_LOCAL},
    {"nil", TokenID::TK_NIL},
    {"not", TokenID::TK_NOT},
    {"or", TokenID::TK_OR},
    {"repeat", TokenID::TK_REPEAT},
    {"return", TokenID::TK_RETURN},
    {"then", TokenID::TK_THEN},
    {"true", TokenID::TK_TRUE},
    {"until", TokenID::TK_UNTIL},
    {"while", TokenID::TK_WHILE},
    {"//", TokenID::TK_IDIV},
    {"..", TokenID::TK_CONCAT},
    {"...", TokenID::TK_DOTS},
    {"==", TokenID::TK_EQ},
    {">=", TokenID::TK_GE},
    {"<=", TokenID::TK_LE},
    {"~=", TokenID::TK_NE},
    {"<<", TokenID::TK_SHL},
    {">>", TokenID::TK_SHR},
    {"::", TokenID::TK_DBCOLON},
};

std::ostream& operator<<(std::ostream& s, const Token& t) {
    auto id = static_cast<typename std::underlying_type<TokenID>::type>(t.id);
    auto index = id - UCHAR_MAX;
    s<<"Token Type"<< id << ':' << tk_type_str[index]<<std::endl;
    return s;
}



bool Token::operator==(const Token& rhs) {
    if(id != rhs.id){
        return false;
    }
    else if(id < static_cast<Token::TokenIDType>(TokenID::TK_FLT)) {
        return true;
    }
    else {
        return info == rhs.info;             
    }
}

