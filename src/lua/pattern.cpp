#include "lua/pattern.h"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ys
{
    namespace lua
    {
        namespace {

        // -------------------------------------------------------------------
        // Character class predicates
        // -------------------------------------------------------------------

        bool is_alpha(unsigned char c)  { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
        bool is_digit(unsigned char c)  { return c >= '0' && c <= '9'; }
        bool is_lower(unsigned char c)  { return c >= 'a' && c <= 'z'; }
        bool is_upper(unsigned char c)  { return c >= 'A' && c <= 'Z'; }
        bool is_alnum(unsigned char c)  { return is_alpha(c) || is_digit(c); }
        bool is_xdigit(unsigned char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
        bool is_space(unsigned char c)  { return c == ' ' || (c >= 9 && c <= 13); }  // space, \t\n\v\f\r
        bool is_punct(unsigned char c)  {
            return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) ||
                   (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
        }
        bool is_cntrl(unsigned char c)  { return c < 32 || c == 127; }
        bool is_graph(unsigned char c)  { return c >= 33 && c <= 126; }
        bool is_print(unsigned char c)  { return c >= 32 && c <= 126; }

        // Match a single character class code (after %): %a, %d, etc.
        // Upper-case = complement.
        bool class_match(unsigned char c, char cls)
        {
            switch (cls) {
            case 'a': return is_alpha(c);
            case 'A': return !is_alpha(c);
            case 'c': return is_cntrl(c);
            case 'C': return !is_cntrl(c);
            case 'd': return is_digit(c);
            case 'D': return !is_digit(c);
            case 'g': return is_graph(c);
            case 'G': return !is_graph(c);
            case 'l': return is_lower(c);
            case 'L': return !is_lower(c);
            case 'p': return is_punct(c);
            case 'P': return !is_punct(c);
            case 's': return is_space(c);
            case 'S': return !is_space(c);
            case 'u': return is_upper(c);
            case 'U': return !is_upper(c);
            case 'w': return is_alnum(c);
            case 'W': return !is_alnum(c);
            case 'x': return is_xdigit(c);
            case 'X': return !is_xdigit(c);
            case 'z': return c == 0;       // deprecated alias for \0
            case 'Z': return c != 0;
            default:  return false;        // not a class — it's a literal escape
            }
        }

        // Is this char a class code? (Used to distinguish %a from \. etc.)
        bool is_class_code(char c)
        {
            // NUL is NEVER a class code (it's a literal byte). Must NOT use a
            // C-string range-for here because it would include the NUL terminator.
            static const char codes[] = {'a','A','c','C','d','D','g','G','l','L',
                                         'p','P','s','S','u','U','w','W','x','X',
                                         'z','Z'};
            for (char x : codes) if (x == c) return true;
            return false;
        }

        // -------------------------------------------------------------------
        // Set parsing: [set] or [^set]
        // -------------------------------------------------------------------

        // Check if byte c matches a bracket set starting at pat[p] (which must
        // be past the opening '[' and optional '^'). `p_end` is one-past the
        // closing ']'.
        bool set_match(unsigned char c, std::string_view pat,
                       std::size_t p, std::size_t p_end)
        {
            // p points to first char after '[' (and optional '^')
            // p_end points to the ']'
            bool complemented = false;
            if (p < p_end && pat[p] == '^') {
                complemented = true;
                ++p;
            }
            // A ']' right after '[' or '[^' is a literal (not the close).
            bool first = true;
            bool found = false;
            while (p < p_end && (first || pat[p] != ']')) {
                first = false;
                unsigned char ch1 = static_cast<unsigned char>(pat[p]);
                if (ch1 == '%' && p + 1 < p_end) {
                    // %escape inside set
                    char esc = pat[p + 1];
                    if (is_class_code(esc)) {
                        if (class_match(c, esc)) found = true;
                        p += 2;
                        continue;
                    }
                    // Literal escaped char
                    ch1 = static_cast<unsigned char>(esc);
                    p += 2;
                } else {
                    ++p;
                }
                // Range?
                if (p < p_end && pat[p] == '-' && p + 1 < p_end &&
                    pat[p + 1] != ']') {
                    unsigned char ch2 = static_cast<unsigned char>(pat[p + 1]);
                    if (ch2 == '%' && p + 2 < p_end) {
                        ch2 = static_cast<unsigned char>(pat[p + 2]);
                        p += 3;
                    } else {
                        p += 2;
                    }
                    if (c >= ch1 && c <= ch2) found = true;
                } else {
                    if (c == ch1) found = true;
                }
            }
            return complemented ? !found : found;
        }

        // Find the position of the matching ']' for a '[' at pat[p].
        // Returns position of ']' or throws if unterminated.
        std::size_t find_set_end(std::string_view pat, std::size_t p)
        {
            // p points to '['
            ++p;
            // Skip optional '^'
            if (p < pat.size() && pat[p] == '^') ++p;
            // First ']' is literal
            if (p < pat.size() && pat[p] == ']') ++p;
            while (p < pat.size() && pat[p] != ']') {
                if (pat[p] == '%' && p + 1 < pat.size()) ++p;
                ++p;
            }
            if (p >= pat.size())
                throw PatternError("malformed pattern (missing ']')");
            return p;
        }

        // -------------------------------------------------------------------
        // The recursive matcher
        // -------------------------------------------------------------------

        // State carried through the match recursion. Follows reference Lua's
        // design: cap_level is the TOTAL number of captures (open + closed),
        // never decremented on close. Each `(` pushes one capture; `)` marks
        // the last unfinished one as closed (sets len). Back-references index
        // into the caps array by capture order.
        struct MatchState {
            std::string_view subj;
            std::string_view pat;
            std::vector<Capture> caps;
            int cap_level{0};  // total captures (open + closed)

            void push_cap(std::size_t s, std::size_t len)
            {
                if (cap_level >= 32)
                    throw PatternError("too many captures");
                if (static_cast<int>(caps.size()) <= cap_level)
                    caps.push_back({s, len});
                else
                    caps[cap_level] = {s, len};
                ++cap_level;
            }

            // Close the most recent unfinished capture at position s.
            int close_cap(std::size_t s)
            {
                int l = cap_level - 1;
                while (l >= 0 && caps[l].len != CAP_UNFINISHED) --l;
                if (l < 0)
                    throw PatternError("invalid pattern capture");
                caps[l].len = s - caps[l].start;
                return l;
            }
        };

        // Forward declaration of the core match function.
        static std::optional<std::size_t> match_rec(MatchState& ms,
            std::size_t s, std::size_t p);

        // Match a single "pattern item" at subject position s. A pattern item
        // is a class followed by an optional quantifier. Returns the pattern
        // position after the item+quantifier, and sets `matched_len` to the
        // number of bytes consumed from the subject (0 for empty match).
        //
        // `single_match_at` checks if the FIRST byte of the item class matches.
        // Returns true/false.
        static bool single_match(unsigned char c, std::string_view pat,
                                 std::size_t p, std::size_t p_end)
        {
            // p points to start of class, p_end is one-past class (before quantifier)
            unsigned char ch = static_cast<unsigned char>(pat[p]);
            if (ch == '.') return true;  // any byte
            if (ch == '%') {
                if (p + 1 >= p_end) return false;  // shouldn't happen (validated)
                char esc = pat[p + 1];
                if (is_class_code(esc))
                    return class_match(c, esc);
                // literal escape
                return c == static_cast<unsigned char>(esc);
            }
            if (ch == '[') {
                // p should point to '[', p_end is after ']'
                // Skip past '[' and optional '^'
                std::size_t sp = p + 1;
                return set_match(c, pat, sp, p_end);
            }
            // literal
            return c == ch;
        }

        // Given pat position p (at a class start), return the position
        // one-past the class (before any quantifier). Also handles %b and %f.
        static std::size_t class_end(std::string_view pat, std::size_t p)
        {
            switch (pat[p]) {
            case '%':
                if (p + 1 >= pat.size())
                    throw PatternError("malformed pattern (ends with '%')");
                return p + 2;
            case '[': {
                std::size_t end = find_set_end(pat, p);
                return end + 1;  // past ']'
            }
            default:
                return p + 1;
            }
        }

        // Max expand for greedy/lazy quantifiers: try matching the rest of the
        // pattern after consuming `count` copies of the class, from `s+count`.
        //
        // greedy: try from max down to 0; first success wins
        // lazy (-): try from 0 up to max; first success wins
        static std::optional<std::size_t> max_expand(MatchState& ms,
            std::size_t s, std::size_t p, std::size_t p_end,
            std::size_t max_count, bool lazy)
        {
            std::size_t count = max_count;
            if (lazy) {
                for (std::size_t c = 0; c <= max_count; ++c) {
                    int saved = ms.cap_level;
                    auto r = match_rec(ms, s + c, p_end);
                    if (r) return r;
                    ms.cap_level = saved;
                }
            } else {
                while (true) {
                    int saved = ms.cap_level;
                    auto r = match_rec(ms, s + count, p_end);
                    if (r) return r;
                    ms.cap_level = saved;
                    if (count == 0) break;
                    --count;
                }
            }
            return std::nullopt;
        }

        // Core recursive match. Returns the subject end position on success.
        static std::optional<std::size_t> match_rec(MatchState& ms,
            std::size_t s, std::size_t p)
        {
            std::string_view pat = ms.pat;

            // End of pattern -> success
            if (p >= pat.size()) return s;

            // End anchor: $ must be last
            if (pat[p] == '$' && p + 1 == pat.size())
                return s == ms.subj.size() ? std::optional<std::size_t>(s)
                                           : std::nullopt;

            // Capture open: (
            if (pat[p] == '(') {
                int saved = ms.cap_level;
                if (p + 1 < pat.size() && pat[p + 1] == ')') {
                    // Position capture ()
                    ms.push_cap(s, CAP_POSITION);
                    auto r = match_rec(ms, s, p + 2);
                    if (!r) ms.cap_level = saved;
                    return r;
                }
                ms.push_cap(s, CAP_UNFINISHED);
                auto r = match_rec(ms, s, p + 1);
                if (!r) ms.cap_level = saved;
                return r;
            }

            // Capture close: )
            if (pat[p] == ')') {
                int idx = ms.close_cap(s);
                auto r = match_rec(ms, s, p + 1);
                if (!r) {
                    // Undo the close.
                    ms.caps[static_cast<std::size_t>(idx)].len =
                        CAP_UNFINISHED;
                }
                return r;
            }

            // %bXY balanced match
            if (pat[p] == '%' && p + 1 < pat.size() && pat[p + 1] == 'b') {
                if (p + 3 >= pat.size())
                    throw PatternError("malformed pattern (missing arguments to '%b')");
                char open_ch = pat[p + 2];
                char close_ch = pat[p + 3];
                if (s >= ms.subj.size() || ms.subj[s] != open_ch)
                    return std::nullopt;
                int depth = 1;
                std::size_t i = s + 1;
                while (i < ms.subj.size()) {
                    // Check close FIRST so %b'' (open==close) works correctly.
                    if (ms.subj[i] == close_ch) {
                        --depth;
                        if (depth == 0)
                            return match_rec(ms, i + 1, p + 4);
                    } else if (ms.subj[i] == open_ch) {
                        ++depth;
                    }
                    ++i;
                }
                return std::nullopt;
            }

            // %f[set] frontier
            if (pat[p] == '%' && p + 1 < pat.size() && pat[p + 1] == 'f') {
                if (p + 2 >= pat.size() || pat[p + 2] != '[')
                    throw PatternError("missing '[' after '%f' in pattern");
                std::size_t set_end = find_set_end(pat, p + 2);
                // set content is pat[p+3 .. set_end-1] (past '[')
                unsigned char prev = (s == 0) ? '\0'
                    : static_cast<unsigned char>(ms.subj[s - 1]);
                unsigned char curr = (s < ms.subj.size())
                    ? static_cast<unsigned char>(ms.subj[s]) : '\0';
                bool prev_in = set_match(prev, pat, p + 3, set_end);
                bool curr_in = set_match(curr, pat, p + 3, set_end);
                if (!prev_in && curr_in)
                    return match_rec(ms, s, set_end + 1);
                return std::nullopt;
            }

            // %digit: back-reference
            if (pat[p] == '%' && p + 1 < pat.size() &&
                pat[p + 1] >= '0' && pat[p + 1] <= '9') {
                if (pat[p + 1] == '0')
                    throw PatternError("invalid capture index %0");
                int idx = pat[p + 1] - '1';  // %1 = caps[0]
                if (idx >= ms.cap_level)
                    throw PatternError(std::string("invalid capture index %") +
                                       pat[p + 1]);
                Capture& cap = ms.caps[idx];
                if (cap.len == CAP_UNFINISHED)
                    throw PatternError(std::string("invalid capture index %") +
                                       pat[p + 1]);
                // Position captures and empty captures: no text to match.
                if (cap.len == CAP_POSITION || cap.len == 0) {
                    return match_rec(ms, s, p + 2);
                }
                std::size_t cap_len = cap.len;
                // Check the captured text appears at s
                if (s + cap_len > ms.subj.size())
                    return std::nullopt;
                if (std::memcmp(&ms.subj[s], &ms.subj[cap.start], cap_len) != 0)
                    return std::nullopt;
                return match_rec(ms, s + cap_len, p + 2);
            }

            // Standard class item (literal, %class, [set], .)
            std::size_t p_end = class_end(pat, p);
            // Check for quantifier after the class
            bool has_quant = (p_end < pat.size());
            char quant = has_quant ? pat[p_end] : '\0';

            if (has_quant && (quant == '*' || quant == '+' || quant == '-' || quant == '?')) {
                // Count max repetitions
                std::size_t count = 0;
                while (s + count < ms.subj.size() &&
                       single_match(static_cast<unsigned char>(ms.subj[s + count]),
                                    pat, p, p_end)) {
                    ++count;
                }
                if (quant == '+') {
                    // + = match one, then greedy *. We already verified count
                    // >= 1 (the first char matches). Consume it, then try
                    // greedily from s+1 with count-1 additional repetitions.
                    if (count == 0) return std::nullopt;
                    return max_expand(ms, s + 1, p, p_end + 1, count - 1, false);
                }
                if (quant == '*') {
                    return max_expand(ms, s, p, p_end + 1, count, false);
                }
                if (quant == '-') {
                    return max_expand(ms, s, p, p_end + 1, count, true);
                }
                if (quant == '?') {
                    // Optional: try with match (if any), then without
                    if (count >= 1) {
                        auto r = match_rec(ms, s + 1, p_end + 1);
                        if (r) return r;
                    }
                    return match_rec(ms, s, p_end + 1);
                }
            }

            // No quantifier: must match exactly one
            if (s < ms.subj.size() &&
                single_match(static_cast<unsigned char>(ms.subj[s]),
                             pat, p, p_end)) {
                return match_rec(ms, s + 1, p_end);
            }
            return std::nullopt;
        }

        } // anonymous namespace

        // -------------------------------------------------------------------
        // Public API
        // -------------------------------------------------------------------

        std::optional<MatchResult> pattern_at(
            std::string_view subj, std::size_t init,
            std::string_view pat)
        {
            // Validate basic pattern structure.
            if (pat.empty())
                throw PatternError("empty pattern");

            // Check for anchor ^
            bool anchored = !pat.empty() && pat[0] == '^';
            std::string_view real_pat = anchored
                ? pat.substr(1) : pat;

            MatchState ms{subj, real_pat, {}, 0};
            auto end = match_rec(ms, init, 0);

            if (!end) return std::nullopt;

            // Verify all captures are closed
            for (int i = 0; i < ms.cap_level; ++i)
                if (ms.caps[i].len == CAP_UNFINISHED)
                    throw PatternError("unfinished capture");

            ms.caps.resize(ms.cap_level);
            MatchResult result;
            result.end = *end;
            result.captures = std::move(ms.caps);
            return result;
        }

        std::optional<FindResult> pattern_find(
            std::string_view subj, std::size_t init,
            std::string_view pat, bool plain)
        {
            if (init > subj.size()) init = subj.size();

            // Plain text search (no pattern interpretation)
            if (plain) {
                if (pat.empty()) {
                    return FindResult{init, init, {}};
                }
                if (init + pat.size() > subj.size()) return std::nullopt;
                // memmem-style search
                for (std::size_t i = init; i + pat.size() <= subj.size(); ++i) {
                    if (std::memcmp(&subj[i], pat.data(), pat.size()) == 0)
                        return FindResult{i, i + pat.size(), {}};
                }
                return std::nullopt;
            }

            // Pattern search: anchor only restricts to init
            bool anchored = !pat.empty() && pat[0] == '^';
            std::string_view real_pat = anchored ? pat.substr(1) : pat;

            for (std::size_t i = init; i <= subj.size(); ++i) {
                try {
                    MatchState ms{subj, real_pat, {}, 0};
                    auto end = match_rec(ms, i, 0);
                    if (end) {
                        for (int ci = 0; ci < ms.cap_level; ++ci)
                            if (ms.caps[ci].len == CAP_UNFINISHED)
                                throw PatternError("unfinished capture");
                        // Truncate caps to cap_level
                        ms.caps.resize(ms.cap_level);
                        FindResult fr;
                        fr.start = i;
                        fr.end = *end;
                        fr.captures = std::move(ms.caps);
                        return fr;
                    }
                } catch (const PatternError&) {
                    throw;  // propagate malformed pattern
                }
                if (anchored) break;  // ^ means only try at init
            }
            return std::nullopt;
        }

    } // namespace lua
} // namespace ys
