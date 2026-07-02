// Pattern engine unit tests — pure C++ tests over std::string_view.
// No LuaValue, no lex/parse/eval. Tests the matcher directly.

#include "doctest.h"

#include <string>

#include "lua/pattern.h"

using namespace ys::lua;

namespace {
// Shorthand: check that pattern_find finds pat in subj starting at init.
bool find_ok(std::string_view subj, std::string_view pat,
             std::size_t init = 0, bool plain = false)
{
    return pattern_find(subj, init, pat, plain).has_value();
}

// Shorthand: get the FindResult (assumes a match exists).
FindResult find_res(std::string_view subj, std::string_view pat,
                    std::size_t init = 0)
{
    auto r = pattern_find(subj, init, pat, false);
    REQUIRE(r);
    return *r;
}
} // namespace

// =====================================================================
// Literal patterns
// =====================================================================

TEST_CASE("pattern: literal find")
{
    CHECK(find_ok("hello world", "world"));
    CHECK_FALSE(find_ok("hello", "xyz"));
    // Empty pattern matches at position 0
    CHECK(find_ok("hello", "", 0, true));
    // Plain text search (no pattern interpretation)
    auto r = find_res("a.b.c", "%.");  // % escapes dot
    CHECK(r.start == 1);
    CHECK(r.end == 2);
    // Plain mode
    auto rp = pattern_find("a.b.c", 0, ".", true);
    REQUIRE(rp);
    CHECK(rp->start == 1);
}

TEST_CASE("pattern: literal special chars are escaped by %")
{
    // %. matches a literal dot
    auto r = find_res("a.b", "%.");
    CHECK(r.start == 1);
    CHECK(r.end == 2);
    // %$ matches literal dollar
    CHECK(find_ok("a$b", "%$"));
}

// =====================================================================
// Character classes
// =====================================================================

TEST_CASE("pattern: character classes")
{
    CHECK(find_ok("abc123", "%d"));
    CHECK(find_ok("abc", "%a"));
    CHECK(find_ok("  x", "%s"));
    CHECK_FALSE(find_ok("abc", "%d"));
    // . matches any byte
    auto r = find_res("abc", ".");
    CHECK(r.start == 0);
    // Complement classes
    CHECK(find_ok("123", "%D") == false);  // digits are not %D
    CHECK(find_ok("abc", "%A") == false);  // alpha are not %A
    CHECK(find_ok("   ", "%S") == false);
}

TEST_CASE("pattern: [set] ranges")
{
    CHECK(find_ok("hello", "[aeiou]"));  // contains vowel
    CHECK_FALSE(find_ok("xyz", "[aeiou]"));
    auto r = find_res("abc5", "[%d]");
    CHECK(r.start == 3);
    // Complement set
    CHECK(find_ok("a1b2", "[^%d]"));  // non-digit exists
}

// =====================================================================
// Quantifiers
// =====================================================================

TEST_CASE("pattern: * (zero or more, greedy)")
{
    auto r = find_res("aaa", "a*");
    CHECK(r.start == 0);
    CHECK(r.end == 3);
    // Zero matches
    r = find_res("bbb", "a*");
    CHECK(r.start == 0);
    CHECK(r.end == 0);
}

TEST_CASE("pattern: + (one or more, greedy)")
{
    auto r = find_res("aaab", "a+");
    CHECK(r.start == 0);
    CHECK(r.end == 3);
    // No match
    CHECK_FALSE(find_ok("bbb", "a+"));
}

TEST_CASE("pattern: - (zero or more, lazy)")
{
    auto r = find_res("aaa", "a-");
    CHECK(r.start == 0);
    CHECK(r.end == 0);  // lazy: matches 0
    // Lazy with rest of pattern: a-b means "zero+ lazy 'a' then literal 'b'"
    // On "ab": at pos 0, a- tries 0 first -> b matches 'a'? No. a- tries 1 -> b matches subj[1]='b'? Yes.
    r = find_res("ab", "a-b");
    CHECK(r.start == 0);
    CHECK(r.end == 2);
    // On "aXb": a- tries 0 -> b vs 'a' fails; a- tries 1 -> b vs 'X' fails.
    // At pos 2: a- matches 0, b matches 'b'. Match = "b".
    r = find_res("aXb", "a-b");
    CHECK(r.start == 2);
    CHECK(r.end == 3);
}

TEST_CASE("pattern: ? (optional)")
{
    auto r = find_res("ac", "ab?c");
    CHECK(r.start == 0);
    CHECK(r.end == 2);
    r = find_res("abc", "ab?c");
    CHECK(r.start == 0);
    CHECK(r.end == 3);
}

// =====================================================================
// Anchors
// =====================================================================

TEST_CASE("pattern: anchors ^ and $")
{
    CHECK(find_ok("hello", "^hello"));
    CHECK_FALSE(find_ok("xhello", "^hello"));
    CHECK(find_ok("hello", "o$"));
    CHECK_FALSE(find_ok("hello", "x$"));
    // Both anchors
    CHECK(find_ok("exact", "^exact$"));
    CHECK_FALSE(find_ok("exact ", "^exact$"));
}

// =====================================================================
// Captures
// =====================================================================

TEST_CASE("pattern: simple capture")
{
    auto r = find_res("hello world", "(%w+)");
    REQUIRE(r.captures.size() == 1);
    CHECK(r.captures[0].start == 0);
    CHECK(r.captures[0].len == 5);  // "hello"
}

TEST_CASE("pattern: multiple captures")
{
    auto r = find_res("key=value", "(%w+)=(%w+)");
    REQUIRE(r.captures.size() == 2);
    CHECK(r.captures[0].start == 0);
    CHECK(r.captures[0].len == 3);  // "key"
    CHECK(r.captures[1].start == 4);
    CHECK(r.captures[1].len == 5);  // "value"
}

TEST_CASE("pattern: position capture ()")
{
    auto r = find_res("hello", "()l");
    REQUIRE(r.captures.size() == 1);
    CHECK(is_position_cap(r.captures[0]));  // position capture
    CHECK(r.captures[0].start == 2);  // first 'l' at position 2
}

TEST_CASE("pattern: nested captures")
{
    auto r = find_res("abc", "((a)(b)(c))");
    REQUIRE(r.captures.size() == 4);
    CHECK(r.captures[0].start == 0);  // whole "abc"
    CHECK(r.captures[0].len == 3);
    CHECK(r.captures[1].start == 0);  // "a"
    CHECK(r.captures[1].len == 1);
}

// =====================================================================
// Back-references
// =====================================================================

TEST_CASE("pattern: back-reference in pattern")
{
    // Match a doubled word: "hello hello"
    auto r = find_res("hello hello", "(%w+) %1");
    REQUIRE(r.captures.size() == 1);
    CHECK(r.captures[0].start == 0);
    CHECK(r.captures[0].len == 5);
    // Should NOT match different words
    CHECK_FALSE(find_ok("hello world", "(%w+) %1"));
}

// =====================================================================
// %b balanced
// =====================================================================

TEST_CASE("pattern: %b balanced match")
{
    auto r = find_res("(a(b)c)", "%b()");
    CHECK(r.start == 0);
    CHECK(r.end == 7);  // entire "(a(b)c)"
    CHECK(r.captures.empty());
    // Nested
    r = find_res("x((y))z", "%b()");
    CHECK(r.start == 1);
    CHECK(r.end == 6);  // "((y))"
}

TEST_CASE("pattern: %b with non-paren delimiters")
{
    auto r = find_res("[a[b]c]", "%b[]");
    CHECK(r.start == 0);
    CHECK(r.end == 7);
}

// =====================================================================
// %f frontier
// =====================================================================

TEST_CASE("pattern: %f[set] frontier")
{
    // Match a digit at the boundary of non-digit and digit
    auto r = find_res("abc123", "%f[%d]%d+");
    CHECK(r.start == 3);
    CHECK(r.end == 6);
}

// =====================================================================
// Error cases
// =====================================================================

TEST_CASE("pattern: malformed pattern errors")
{
    auto try_match = [](const char* p) {
        try { pattern_find("a", 0, p, false); return false; }
        catch (const PatternError&) { return true; }
    };
    CHECK(try_match("("));
    CHECK(try_match(")"));
    CHECK(try_match("[abc"));
    CHECK(try_match("%b"));
    CHECK(try_match("%"));
    CHECK(try_match("%f"));
}

// =====================================================================
// Edge cases
// =====================================================================

TEST_CASE("pattern: init argument")
{
    // Find from position 3
    auto r = find_res("aaa", "a", 2);
    CHECK(r.start == 2);
    CHECK(r.end == 3);
}

TEST_CASE("pattern: empty match handling")
{
    // gsub-style empty match behavior: " *" on "a b cd"
    // The first match at pos 0 is empty (no space there).
    auto r = pattern_find("a b cd", 0, " *", false);
    REQUIRE(r);
    // Empty match at position 0
    CHECK(r->start == 0);
    CHECK(r->end == 0);
}
