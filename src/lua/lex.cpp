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

Tokenizer::Tokenizer(std::string input)
    : m_grammar(lexconv::make_grammar())
    , m_input(std::move(input))
    , m_context(m_input)
{
    // Long-bracket comment body: the matcher in lex_conv.h handles the
    // entire [=*[ ... ]=*] construct including body consumption; nothing
    // to do here — no token is produced.

    // Long-bracket string body: the matcher consumed [=*[ ... ]=*]; on_match
    // computes the body range and sets the TK_STRING token.
    m_grammar["long_bracket_start"].on_match(
        [this](Context& c, const Context::ParseTreeNodePtr& node) {
            auto sp_start = node->start_offset;
            auto sp_end   = node->end_offset;
            // Recompute level from the opening bracket characters.
            std::size_t level = 0;
            std::size_t i = sp_start + 1; // skip first '['
            while (i < sp_end && c.at(i) == '=') { ++level; ++i; }
            // i now points at the second '['; body starts after it, plus
            // an optional newline that the matcher already skipped.
            std::size_t body_start = i + 1;
            if (body_start < sp_end) {
                if (c.at(body_start) == '\r') { ++body_start;
                    if (body_start < sp_end && c.at(body_start) == '\n') ++body_start; }
                else if (c.at(body_start) == '\n') ++body_start;
            }
            // Closing bracket: ']' + level×'=' + ']' before sp_end.
            std::size_t body_end = sp_end - level - 2;
            m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
            m_token_buf.info = c.input().slice(body_start, body_end - body_start);
            m_token_buf.start = sp_start;
            m_token_buf.end = sp_end;
        });

    m_grammar["ops"].on_match(
        [this](Context& c, const Context::ParseTreeNodePtr& node) {
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
        });

    m_grammar["name"].on_match(
        [this](Context& c, const Context::ParseTreeNodePtr& node) {
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
        });

    m_grammar["string_literal"].on_match(
        [this](Context& c, const Context::ParseTreeNodePtr& node) {
            // long_bracket_start on_match already produced the token; only
            // handle the single/double-quoted forms.
            if (m_token_buf.id == -1) {
                m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_STRING);
                auto raw = c.input().slice(node->start_offset + 1,
                                    (node->end_offset - node->start_offset) - 2);
                m_token_buf.info = decode_escapes(raw);
                m_token_buf.start = node->start_offset;
                m_token_buf.end = node->end_offset;
            }
        });

    m_grammar["numeral"].on_match(
        [this](Context& c, const Context::ParseTreeNodePtr& node) {
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
                    // Hex integer (no dot, no exponent): Lua 5.4 says these
                    // ALWAYS denote an integer value; on overflow they wrap
                    // around (two's complement), they do NOT become floats.
                    // std::from_chars does NOT wrap on overflow, so parse
                    // manually, accumulating mod 2^64 to match Lua semantics.
                    unsigned long long uval = 0;
                    for (const char* p = hfirst; p < last; ++p) {
                        char c = *p;
                        unsigned digit;
                        if (c >= '0' && c <= '9') digit = c - '0';
                        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                        else break;
                        uval = uval * 16 + digit;
                    }
                    m_token_buf.id = static_cast<Token::TokenIDType>(TokenID::TK_INT);
                    m_token_buf.info = static_cast<long long>(uval);
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
        });
}

std::vector<Token> Tokenizer::tokenize()
{
    std::vector<Token> out;

    // Lua 5.4 §3.1: if the first line of a chunk begins with '#', that whole
    // line is ignored (the Unix shebang / "#!" convention, but the rule is
    // broader — any '#' as the file's very first character). The grammar is
    // stateless and cannot express "first line only", so handle it here before
    // the main scan. No diagnostic, no token — the line is simply consumed.
    if (m_context.input_size() > 0 && m_context.at(0) == '#') {
        std::size_t i = 0;
        // Stop at any newline byte (\n Unix, \r old-Mac, or the \r of \r\n).
        while (i < m_context.input_size() &&
               m_context.at(i) != '\n' && m_context.at(i) != '\r') ++i;
        if (i < m_context.input_size()) {
            // Consume the line terminator: CRLF is two bytes, \n or \r one.
            if (m_context.at(i) == '\r' && i + 1 < m_context.input_size() &&
                m_context.at(i + 1) == '\n') {
                i += 2;
            } else {
                ++i;
            }
        }
        m_context.reset(i);
    }

    while (!m_context.ended()) {
        auto tok_start = m_context.mark();
        m_token_buf = Token{}; // clear scratch: id == -1, start/end == 0
        m_token_buf.start = tok_start;

        auto result = m_grammar.parse_ast("token", m_context);
        if (!result) {
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
            // A real token was produced (via on_match). Long-bracket comments
            // and whitespace match "token" but produce no token, so we only
            // emit when an on_match actually wrote one.
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
