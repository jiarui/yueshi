#include "lua/lex.h"
#include <type_traits>
#include <string>
#include <string_view>
#include <iostream>
using namespace peg;
using namespace ys::lua;

// ---------------------------------------------------------------------------
// Escape-sequence decoding for short string literals (Lua 5.4 semantics).
// Long-bracket strings ([[ ... ]]) are NOT decoded; they pass through raw.
//
// Returns the decoded bytes. Throws nothing; malformed inputs (truncated
// escapes, overlarge \ddd) are passed through literally to match Lua's
// "lexer error" posture — the grammar already rejects syntactically invalid
// escapes, so by the time we get here the structure is well-formed.
// ---------------------------------------------------------------------------
namespace {
// Encode a single Unicode codepoint as UTF-8. Used for \u{XXXX}.
std::string encode_utf8(unsigned cp)
{
    std::string out;
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

std::string decode_escapes(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        // We are at a backslash; the grammar guarantees a following escape
        // code, so peeking raw[i+1] is safe.
        char e = raw[++i];
        switch (e) {
        case 'a':  out += '\a'; break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'v':  out += '\v'; break;
        case '\\': out += '\\'; break;
        case '"':  out += '"';  break;
        case '\'': out += '\''; break;
        case '\n': out += '\n'; break; // backslash-newline line continuation
        case 'x': {
            // Exactly two hex digits follow (enforced by the grammar).
            auto hexval = [](char ch) {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                return ch - 'A' + 10;
            };
            int b = (hexval(raw[++i]) << 4) | hexval(raw[++i]);
            out += static_cast<char>(b);
            break;
        }
        case 'z':
            // Skip following whitespace (incl. newlines) until a non-space.
            while (i + 1 < raw.size()) {
                char s = raw[i + 1];
                if (s == ' ' || s == '\t' || s == '\n' || s == '\r' ||
                    s == '\f' || s == '\v') {
                    ++i;
                } else {
                    break;
                }
            }
            break;
        case 'u': {
            // \u{XXXX} — hex digits until the closing brace (grammar-enforced).
            // raw[i] is 'u'; expect '{' next.
            ++i; // consume '{'
            unsigned cp = 0;
            auto hexval = [](char ch) {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                return ch - 'A' + 10;
            };
            while (raw[i + 1] != '}') {
                cp = (cp << 4) | static_cast<unsigned>(hexval(raw[++i]));
            }
            ++i; // consume '}'
            out += encode_utf8(cp);
            break;
        }
        default:
            // Decimal escape \ddd (1-3 digits). The grammar allows up to 3
            // digits; consume however many digits follow.
            if (e >= '0' && e <= '9') {
                int v = 0;
                int count = 0;
                v = e - '0';
                ++count;
                while (count < 3 && i + 1 < raw.size() &&
                       raw[i + 1] >= '0' && raw[i + 1] <= '9') {
                    v = v * 10 + (raw[++i] - '0');
                    ++count;
                }
                out += static_cast<char>(v);
            } else {
                // Should not happen (grammar rejects unknown escapes), but
                // pass through literally rather than drop data.
                out += '\\';
                out += e;
            }
            break;
        }
    }
    return out;
}
} // namespace

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

Tokenizer::Tokenizer(std::string input)
    : m_grammar(lexconv::make_grammar())
    , m_input(std::move(input))
    , m_context(m_input)
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
                m_token_buf.info = c.input().slice(body_start, body_end - body_start);
                m_token_buf.start = node->start_offset;
                m_token_buf.end = c.mark();
            }
            return {};
        });

    m_grammar["ops"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            // Owned copy — keep alive for the lookup; do not bind to string_view
            // (slice returns by value, so a string_view over it would dangle).
            auto result = c.input().slice(node->start_offset, node->end_offset - node->start_offset);
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
            m_token_buf.start = node->start_offset;
            m_token_buf.end = node->end_offset;
            return {};
        });

    m_grammar["name"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            auto result = c.input().slice(node->start_offset, node->end_offset - node->start_offset);
            auto iter = str2tkid.find(result);
            if (iter == str2tkid.end()) {
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_NAME);
                m_token_buf.info = result;
            } else {
                m_token_buf.id = static_cast<Token::TokenIDType>(iter->second);
            }
            m_token_buf.start = node->start_offset;
            m_token_buf.end = node->end_offset;
            return {};
        });

    m_grammar["string_literal"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            // long_bracket_start already produced the token; only handle the
            // single/double-quoted forms (strip the surrounding quote, decode
            // escapes).
            if (m_token_buf.id == -1) {
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
                auto raw = c.input().slice(node->start_offset + 1,
                                    (node->end_offset - node->start_offset) - 2);
                m_token_buf.info = decode_escapes(raw);
                m_token_buf.start = node->start_offset;
                m_token_buf.end = node->end_offset;
            }
            return {};
        });

    m_grammar["numeral"].set_action(
        [this](Context& c, const Context::ParseTreeNodePtr& node) -> std::monostate {
            auto result = c.input().slice(node->start_offset, node->end_offset - node->start_offset);

            // Classify by lexical shape rather than "try int base-10 then fall
            // back to float": the latter mis-routes every hex literal and every
            // literal whose mantissa overflows int (Lua re-reads those as float).
            const auto* first = result.data();
            const auto* last = result.data() + result.size();
            const bool is_hex = result.size() >= 2 &&
                result[0] == '0' && (result[1] == 'x' || result[1] == 'X');
            const bool has_dot = result.find('.') != std::string::npos;
            // For hex literals the binary exponent is p/P; for decimal it is
            // e/E. Only the relevant letter marks a float in each regime.
            const bool has_exp = is_hex
                ? result.find_first_of("pP") != std::string::npos
                : result.find_first_of("eE") != std::string::npos;

            if (is_hex) {
                // std::from_chars does NOT consume the 0x prefix for integer
                // parsing (base 16 expects bare hex digits), and while the
                // chars_format::hex overload does accept it, stripping it
                // uniformly keeps both paths consistent.
                const auto* hfirst = first + 2;
                if (has_dot || has_exp) {
                    // Hex float: mantissa in hex, p-exponent (power of 2).
                    double dval{};
                    std::from_chars(hfirst, last, dval, std::chars_format::hex);
                    m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_FLT);
                    m_token_buf.info = dval;
                } else {
                    long long ivalue{};
                    auto ret = std::from_chars(hfirst, last, ivalue, 16);
                    // Lua re-reads an out-of-range hex integer as a float.
                    if (ret.ec == std::errc::result_out_of_range) {
                        double dval{};
                        std::from_chars(hfirst, last, dval, std::chars_format::hex);
                        m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_FLT);
                        m_token_buf.info = dval;
                    } else {
                        m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_INT);
                        m_token_buf.info = ivalue;
                    }
                }
            } else if (has_dot || has_exp) {
                // Decimal float.
                double dval{};
                std::from_chars(first, last, dval);
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_FLT);
                m_token_buf.info = dval;
            } else {
                // Decimal integer. Lua re-reads an out-of-range integer
                // literal as a float, so check ec and fall back.
                long long ivalue{};
                auto ret = std::from_chars(first, last, ivalue);
                if (ret.ec == std::errc::result_out_of_range) {
                    double dval{};
                    std::from_chars(first, last, dval);
                    m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_FLT);
                    m_token_buf.info = dval;
                } else {
                    m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_INT);
                    m_token_buf.info = ivalue;
                }
            }
            m_token_buf.start = node->start_offset;
            m_token_buf.end = node->end_offset;
            return {};
        });
}

std::vector<Token> Tokenizer::tokenize()
{
    std::vector<Token> out;
    while (!m_context.ended()) {
        auto tok_start = m_context.mark();
        m_token_buf = Token{}; // clear scratch: id == -1, start/end == 0
        m_token_buf.start = tok_start;

        bool ok = m_grammar.parse("token", m_context);
        if (!ok) {
            // Lexing failure. peglib has already recorded the furthest failure
            // position + expected set on the context; take_error() retrieves it.
            // Emit a sentinel so the caller can detect the error.
            Token err;
            err.id = -1;
            err.start = tok_start;
            err.end = m_context.mark();
            out.push_back(std::move(err));
            break;
        }
        if (m_token_buf.id != -1) {
            // A real token was produced (name/numeral/string/ops). Long-bracket
            // comments and whitespace match "token" but produce no token, so we
            // only emit when an action actually wrote one.
            out.push_back(std::move(m_token_buf));
        }
        // else: whitespace or comment consumed; keep scanning.
    }
    // Trailing EOS sentinel marks end-of-stream for the parser.
    Token eos;
    eos.id = static_cast<Token::TokenIDType>(TokenID::TK_EOS);
    eos.start = m_context.mark();
    eos.end = m_context.mark();
    out.push_back(std::move(eos));
    return out;
}

std::optional<std::string> Tokenizer::take_error()
{
    auto diag = m_context.take_error();
    if (!diag) return std::nullopt;
    peg::SourceMap map{std::string_view{m_input}};
    return diag->format(map, "input");
}

#define STR_ELEMENT(p) #p

// Keep in EXACT enum order (lex.h). Every TokenID >= TK_AND must have an entry
// so operator<< never indexes out of bounds.
const static std::vector<const char *> tk_type_str = {
    STR_ELEMENT(TokenID::TK_AND),
    STR_ELEMENT(TokenID::TK_BREAK),
    STR_ELEMENT(TokenID::TK_DO),
    STR_ELEMENT(TokenID::TK_ELSE),
    STR_ELEMENT(TokenID::TK_ELSEIF),
    STR_ELEMENT(TokenID::TK_END),
    STR_ELEMENT(TokenID::TK_FALSE),
    STR_ELEMENT(TokenID::TK_FOR),
    STR_ELEMENT(TokenID::TK_FUNCTION),
    STR_ELEMENT(TokenID::TK_GOTO),
    STR_ELEMENT(TokenID::TK_IF),
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
    {"do", TokenID::TK_DO},
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

std::ostream& ys::lua::operator<<(std::ostream& s, const Token& t) {
    auto id = static_cast<typename std::underlying_type<TokenID>::type>(t.id);
    // TK_AND == UCHAR_MAX + 1 is the enum's base offset; tk_type_str[0] is
    // TK_AND, so the index is id - (UCHAR_MAX + 1), NOT id - UCHAR_MAX.
    auto index = id - (UCHAR_MAX + 1);
    s << "Token Type" << id << ':' << tk_type_str[index] << std::endl;
    return s;
}

bool ys::lua::Token::operator==(const Token& rhs) const {
    if(id != rhs.id){
        return false;
    }
    else if(id < static_cast<Token::TokenIDType>(TokenID::TK_FLT)) {
        // Operators / keywords / single-char terminals carry no payload.
        return true;
    }
    else {
        return info == rhs.info;
    }
}
