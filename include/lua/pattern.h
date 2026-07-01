#pragma once

// yueshi Lua pattern engine (M3.1): a hand-rolled recursive matcher for Lua
// 5.4 patterns. NOT peglib-based — Lua patterns are not PEG (they have
// captures, back-refs %1 inside the pattern, %b balanced, %f frontier that
// don't map to PEG combinators).
//
// The engine is pure C++ (no LuaValue dependency) so it can be unit-tested
// in isolation. The string library wrappers (strlib.cpp) translate between
// LuaValue and these match results.

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ys
{
    namespace lua
    {
        // A single capture: a slice [start, start+len) of the subject, or a
        // position capture (len==0, value = start as a 1-based index).
        struct Capture {
            std::size_t start;  // 0-based byte offset into subject
            std::size_t len;    // match length (0 = position capture)
        };

        // Result of a successful match at a fixed position.
        struct MatchResult {
            std::size_t end;                // 1-past-last matched byte
            std::vector<Capture> captures;  // may be empty
        };

        // Match a pattern anchored at exactly `init` (0-based). Returns
        // nullopt on failure. Validates the pattern (throws PatternError on
        // malformed input).
        std::optional<MatchResult> pattern_at(
            std::string_view subj, std::size_t init,
            std::string_view pat);

        // A find result: the match can start anywhere from `init` onward.
        struct FindResult {
            std::size_t start;              // 0-based byte offset of match start
            std::size_t end;                // 1-past-last matched byte
            std::vector<Capture> captures;  // may be empty
        };

        // Scan from `init` for the first match. If `plain`, do literal
        // substring search (no pattern interpretation). Returns nullopt if
        // no match.
        std::optional<FindResult> pattern_find(
            std::string_view subj, std::size_t init,
            std::string_view pat, bool plain);

        // Exception type for malformed patterns.
        class PatternError : public std::runtime_error {
        public:
            explicit PatternError(const std::string& msg)
                : std::runtime_error(msg) {}
        };
    }
}
