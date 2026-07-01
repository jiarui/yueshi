// Evaluator unit tests: lex -> parse -> Evaluator::run, assert on the
// returned LuaValues. Mirrors the parser-test pipeline (Tokenizer -> Parser ->
// parse()) and adds the run step. Evaluator output (print) is captured by
// injecting a std::ostringstream.
//
// House style: TEST_CASE("evaluator: ..."), SUBCASE, REQUIRE_MESSAGE. Helpers
// in an anonymous namespace; `using namespace ys::lua;` at the top.

#include "doctest.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include "lua/evaluator.h"
#include "lua/lex.h"
#include "lua/parser.h"
#include "lua/value.h"

using namespace ys::lua;

namespace {

// The standard lex+parse pipeline, shared with the parser tests. On success
// returns the root Chunk; on failure, fails the test with a message.
AstNode parse_ok(std::string_view src)
{
    auto toks = Tokenizer{std::string{src}}.tokenize();
    for (std::size_t i = 0; i + 1 < toks.size(); ++i)
        REQUIRE(toks[i].id != -1);
    Parser p{std::move(toks)};
    auto ast = p.parse();
    REQUIRE_MESSAGE(ast, "expected parse to succeed");
    auto errs = p.take_errors();
    REQUIRE_MESSAGE(errs.empty(), "expected no parse diagnostics");
    return std::move(*ast);
}

// A minimal rig: a Heap + a capture stream + an Evaluator. The Heap owns all
// runtime objects for the run; the Evaluator borrows both.
struct EvalRig {
    Heap heap;
    std::ostringstream out;
    std::unique_ptr<Evaluator> ev;
    EvalRig() : ev(std::make_unique<Evaluator>(heap, out)) {}

    // Run src, returning the chunk's return values.
    ValueVec run(std::string_view src) { return ev->run(parse_ok(src)); }

    // Run and return the (single) scalar return value, or nil if none.
    LuaValue run_scalar(std::string_view src)
    {
        auto v = run(src);
        return v.empty() ? LuaValue::nil() : v.front();
    }
};

// Scalars of each tag, for terse assertions.
long long  as_int(const LuaValue& v)    { REQUIRE(v.is_int()); return v.as_int(); }
double     as_flt(const LuaValue& v)    { REQUIRE(v.is_flt()); return v.as_flt(); }
bool       as_bool(const LuaValue& v)   { REQUIRE(v.is_bool()); return v.as_bool(); }
std::string as_str(const LuaValue& v)   { REQUIRE(v.is_str()); return v.as_str()->data; }

} // namespace

// =====================================================================
// Step 2: literals, Name lookup, local/assign, return
// =====================================================================

TEST_CASE("evaluator: integer literal")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return 42")) == 42);
    CHECK(as_int(g.run_scalar("return 0")) == 0);
    // Note: `return -7` needs the unary-minus operator (Step 3); the literal
    // itself is just `7`. Verified here with a positive literal.
    CHECK(as_int(g.run_scalar("return 7")) == 7);
}

TEST_CASE("evaluator: float literal")
{
    EvalRig g;
    CHECK(as_flt(g.run_scalar("return 3.14")) == 3.14);
}

TEST_CASE("evaluator: string literal (escape-decoded by lexer)")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return \"hello\"")) == "hello");
    CHECK(as_str(g.run_scalar("return \"a\\nb\"")) == "a\nb");   // \n decoded
}

TEST_CASE("evaluator: true / false / nil literals")
{
    EvalRig g;
    CHECK(as_bool(g.run_scalar("return true")) == true);
    CHECK(as_bool(g.run_scalar("return false")) == false);
    CHECK(g.run_scalar("return nil").is_nil());
}

TEST_CASE("evaluator: local declaration + Name lookup")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local x = 5; return x")) == 5);
    CHECK(as_str(g.run_scalar("local s = \"hi\"; return s")) == "hi");
}

TEST_CASE("evaluator: locals default to nil when no initializer")
{
    EvalRig g;
    CHECK(g.run_scalar("local x; return x").is_nil());
}

TEST_CASE("evaluator: multiple locals + multires truncation to scalars")
{
    EvalRig g;
    // local a, b = 1, 2 -> both bound (Step 5 will make the last expr
    // multires-aware; here each slot gets one scalar).
    auto v = g.run("local a, b = 1, 2; return a, b");
    REQUIRE(v.size() == 2);
    CHECK(as_int(v[0]) == 1);
    CHECK(as_int(v[1]) == 2);
}

TEST_CASE("evaluator: scoping — inner local shadows outer")
{
    EvalRig g;
    // do/end is not yet implemented (Step 4), so use a single scope.
    // This test verifies shadowing within one block via reassignment semantics
    // will be revisited when `do` lands. For now: redeclare hides nothing.
    CHECK(as_int(g.run_scalar("local x = 1; local x = 2; return x")) == 2);
}

// =====================================================================
// Step 3: operators + Lua 5.4 int/float number semantics
// =====================================================================

TEST_CASE("evaluator: arithmetic — int results stay int")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return 2 + 3")) == 5);
    CHECK(as_int(g.run_scalar("return 10 - 4")) == 6);
    CHECK(as_int(g.run_scalar("return 6 * 7")) == 42);
    CHECK(g.run_scalar("return 6 * 7").is_int());   // subtype preserved
}

TEST_CASE("evaluator: division is always float")
{
    EvalRig g;
    LuaValue r = g.run_scalar("return 6 / 2");
    CHECK(r.is_flt());
    CHECK(as_flt(r) == 3.0);
}

TEST_CASE("evaluator: any-float operand makes float result")
{
    EvalRig g;
    CHECK(g.run_scalar("return 1 + 2.0").is_flt());
    CHECK(as_flt(g.run_scalar("return 1 + 2.0")) == 3.0);
    CHECK(g.run_scalar("return 2.5 * 4").is_flt());
}

TEST_CASE("evaluator: floor division (//) — int and float")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return 7 // 2")) == 3);
    CHECK(as_int(g.run_scalar("return -7 // 2")) == -4);   // floors toward -inf
    CHECK(as_int(g.run_scalar("return 7 // -2")) == -4);
    CHECK(as_flt(g.run_scalar("return 7.0 // 2")) == 3.0);
    CHECK(g.run_scalar("return 7.0 // 2").is_flt());
}

TEST_CASE("evaluator: modulo (%) — sign follows divisor")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return 7 % 3")) == 1);
    CHECK(as_int(g.run_scalar("return -7 % 3")) == 2);    // -7 - floor(-7/3)*3
    CHECK(as_int(g.run_scalar("return 7 % -3")) == -2);
    CHECK(as_flt(g.run_scalar("return 7.5 % 2")) == 1.5);
}

TEST_CASE("evaluator: power (^) is always float")
{
    EvalRig g;
    LuaValue r = g.run_scalar("return 2 ^ 10");
    CHECK(r.is_flt());
    CHECK(as_flt(r) == 1024.0);
}

TEST_CASE("evaluator: integer overflow wraps (two's-complement)")
{
    EvalRig g;
    // LLONG_MAX + 1 wraps to LLONG_MIN.
    auto v = g.run_scalar("return 9223372036854775807 + 1");
    CHECK(v.is_int());
    CHECK(as_int(v) == (-9223372036854775807LL - 1));   // INT64_MIN
}

TEST_CASE("evaluator: unary minus preserves subtype")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return -5")) == -5);
    CHECK(g.run_scalar("return -5").is_int());
    CHECK(g.run_scalar("return -3.0").is_flt());
}

TEST_CASE("evaluator: bitwise operators — integer-only")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return 0xFF & 0x0F")) == 0x0F);
    CHECK(as_int(g.run_scalar("return 0xF0 | 0x0F")) == 0xFF);
    CHECK(as_int(g.run_scalar("return 0xFF ~ 0x0F")) == 0xF0);
    CHECK(as_int(g.run_scalar("return 1 << 4")) == 16);
    CHECK(as_int(g.run_scalar("return 256 >> 4")) == 16);
    CHECK(as_int(g.run_scalar("return ~0")) == -1);
}

TEST_CASE("evaluator: bitwise rejects floats even if integral")
{
    EvalRig g;
    // 3.0 is a float; bitwise on it must error (no implicit coercion).
    bool threw = false;
    try { (void)g.run_scalar("return 3.0 & 1"); } catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("evaluator: equality — int/float cross-subtype by value")
{
    EvalRig g;
    CHECK(as_bool(g.run_scalar("return 1 == 1.0")) == true);
    CHECK(as_bool(g.run_scalar("return 1 ~= 2")) == true);
    CHECK(as_bool(g.run_scalar("return \"a\" == \"a\"")) == true);
    CHECK(as_bool(g.run_scalar("return nil == nil")) == true);
    CHECK(as_bool(g.run_scalar("return 1 == \"1\"")) == false);   // diff types
}

TEST_CASE("evaluator: ordering — numbers and strings")
{
    EvalRig g;
    CHECK(as_bool(g.run_scalar("return 1 < 2")) == true);
    CHECK(as_bool(g.run_scalar("return 2.5 <= 2.5")) == true);
    CHECK(as_bool(g.run_scalar("return 3 > 2")) == true);
    CHECK(as_bool(g.run_scalar("return \"abc\" < \"abd\"")) == true);
    CHECK(as_bool(g.run_scalar("return 1 < 1.5")) == true);   // cross-subtype
}

TEST_CASE("evaluator: logical and/or short-circuit with values")
{
    EvalRig g;
    // and/or return one of their operands (not a boolean).
    CHECK(as_int(g.run_scalar("return 1 and 2")) == 2);
    CHECK(g.run_scalar("return nil and 1").is_nil());        // nil is falsy -> returned
    CHECK(as_int(g.run_scalar("return 1 or 2")) == 1);
    CHECK(g.run_scalar("return false or nil").is_nil());     // false falsy -> returns nil
    CHECK(as_int(g.run_scalar("return false or 5")) == 5);   // false falsy -> returns 5
}

TEST_CASE("evaluator: not and truthiness")
{
    EvalRig g;
    CHECK(as_bool(g.run_scalar("return not nil")) == true);
    CHECK(as_bool(g.run_scalar("return not false")) == true);
    CHECK(as_bool(g.run_scalar("return not 0")) == false);     // 0 is truthy
    CHECK(as_bool(g.run_scalar("return not \"\"")) == false);  // "" is truthy
}

TEST_CASE("evaluator: string concatenation")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return \"a\" .. \"b\"")) == "ab");
    CHECK(as_str(g.run_scalar("return 1 .. 2")) == "12");      // numbers -> strings
    CHECK(as_str(g.run_scalar("return \"n=\" .. 5")) == "n=5");
}

TEST_CASE("evaluator: number-to-string distinguishes int and float")
{
    EvalRig g;
    // Builtins (tostring) are Step 7; verify number->string via concatenation,
    // which uses the same number_to_string helper.
    CHECK(as_str(g.run_scalar("return \"\" .. 3")) == "3");
    CHECK(as_str(g.run_scalar("return \"\" .. 3.0")) == "3.0");   // float keeps .0
    CHECK(as_str(g.run_scalar("return \"\" .. 0.1")) == "0.1");
}

TEST_CASE("evaluator: length (#) on strings")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return #\"hello\"")) == 5);
    CHECK(as_int(g.run_scalar("return #\"\"")) == 0);
}

TEST_CASE("evaluator: precedence — arithmetic")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return 1 + 2 * 3")) == 7);    // * binds tighter
    CHECK(as_int(g.run_scalar("return (1 + 2) * 3")) == 9);  // parens honored
}

// =====================================================================
// Step 4: control flow
// =====================================================================

TEST_CASE("evaluator: if/elseif/else")
{
    EvalRig g;
    auto run_b = [&](const char* src) { return as_bool(g.run_scalar(src)); };
    CHECK(run_b("if true then return true else return false end") == true);
    CHECK(run_b("if false then return true else return false end") == false);
    CHECK(run_b("if 1 > 2 then return 1 elseif 3 > 2 then return true else return false end") == true);
    CHECK(run_b("local x = 5; if x < 0 then return -1 elseif x == 0 then return 0 else return true end") == true);
}

TEST_CASE("evaluator: if with no matching branch falls through")
{
    EvalRig g;
    CHECK(g.run_scalar("if false then return 1 end").is_nil());   // no return -> nil
    CHECK(as_int(g.run_scalar("if false then return 1 end; return 99")) == 99);
}

TEST_CASE("evaluator: while loop")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local i = 0; local s = 0; while i < 5 do s = s + i; i = i + 1 end; return s")) == 10);
}

TEST_CASE("evaluator: while with break")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local i = 0; while true do if i >= 3 then break end; i = i + 1 end; return i")) == 3);
}

TEST_CASE("evaluator: repeat-until")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local i = 0; repeat i = i + 1 until i >= 4; return i")) == 4);
    // repeat-until condition sees the body's locals.
    CHECK(as_int(g.run_scalar(
        "repeat local x = 7 until x == 7; return 1")) == 1);
}

TEST_CASE("evaluator: numeric for — integer")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local s = 0; for i = 1, 10 do s = s + i end; return s")) == 55);
    CHECK(as_int(g.run_scalar(
        "local s = 0; for i = 10, 1, -2 do s = s + i end; return s")) == 30);  // 10+8+6+4+2
    CHECK(as_int(g.run_scalar(
        "for i = 1, 3 do end; return 1")) == 1);   // empty body ok
}

TEST_CASE("evaluator: numeric for — float step")
{
    EvalRig g;
    auto v = g.run_scalar("local s = 0; for i = 1.0, 2.0, 0.5 do s = s + 1 end; return s");
    CHECK(as_int(v) == 3);   // 1.0, 1.5, 2.0 -> 3 iterations
}

TEST_CASE("evaluator: numeric for variable is local to the loop")
{
    EvalRig g;
    // i should not leak (it's nil after the loop).
    CHECK(g.run_scalar("for i = 1, 1 do end; return i").is_nil());
}

TEST_CASE("evaluator: do/end introduces a scope")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local x = 1; do local x = 2 end; return x")) == 1);
    CHECK(g.run_scalar("do local y = 1 end; return y").is_nil());  // y not visible outside
}

TEST_CASE("evaluator: nested control flow")
{
    EvalRig g;
    // Sum of primes < 10 using nested loops.
    CHECK(as_int(g.run_scalar(
        "local sum = 0; "
        "for n = 2, 9 do "
        "  local prime = true "
        "  for d = 2, n - 1 do if n % d == 0 then prime = false; break end end "
        "  if prime then sum = sum + n end "
        "end; return sum")) == 17);   // 2+3+5+7
}

TEST_CASE("evaluator: break breaks only the innermost loop")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local total = 0; "
        "for i = 1, 3 do "
        "  for j = 1, 3 do if j == 2 then break end; total = total + 1 end "
        "end; return total")) == 3);   // j==1 counted once per outer i
}

TEST_CASE("evaluator: return exits the chunk (not just a block)")
{
    EvalRig g;
    // return must be the last statement; the first one in a block wins.
    auto v = g.run("local x = 1; if x == 1 then return 10 end; return 20");
    REQUIRE(v.size() == 1);
    CHECK(as_int(v[0]) == 10);   // the conditional return fires
}

TEST_CASE("evaluator: return from inside a loop")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "for i = 1, 100 do if i == 42 then return i end end; return 0")) == 42);
}

TEST_CASE("evaluator: assignment to existing local")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local x = 1; x = 5; return x")) == 5);
}

TEST_CASE("evaluator: multiple assignment swaps in one step")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local a, b = 1, 2; a, b = b, a; return a")) == 2);
    CHECK(as_int(g.run_scalar("local a, b = 1, 2; a, b = b, a; return b")) == 1);
}

TEST_CASE("evaluator: assignment creates globals")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("x = 10; return x")) == 10);
}

TEST_CASE("evaluator: <const> local rejects reassignment")
{
    EvalRig g;
    bool threw = false;
    try { (void)g.run_scalar("local x <const> = 1; x = 2"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

// =====================================================================
// Step 5: functions, closures, varargs, calls, multires
// =====================================================================

TEST_CASE("evaluator: function definition + call")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local f = function() return 42 end; return f()")) == 42);
    CHECK(as_int(g.run_scalar("local f = function(x) return x * 2 end; return f(21)")) == 42);
}

TEST_CASE("evaluator: missing arguments default to nil")
{
    EvalRig g;
    CHECK(g.run_scalar("local f = function(a, b) return b end; return f(1)").is_nil());
}

TEST_CASE("evaluator: extra arguments are dropped (no vararg)")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local f = function(a) return a end; return f(1, 2, 3)")) == 1);
}

TEST_CASE("evaluator: multiple return values")
{
    EvalRig g;
    auto v = g.run("local f = function() return 1, 2, 3 end; return f()");
    REQUIRE(v.size() == 3);
    CHECK(as_int(v[0]) == 1);
    CHECK(as_int(v[1]) == 2);
    CHECK(as_int(v[2]) == 3);
}

TEST_CASE("evaluator: varargs (...) collect extra arguments")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local f = function(...) return ... end; return f(1, 2, 3)")) == 1);
    auto v = g.run("local f = function(...) return ... end; return f(10, 20)");
    REQUIRE(v.size() == 2);
    CHECK(as_int(v[0]) == 10);
    CHECK(as_int(v[1]) == 20);
}

TEST_CASE("evaluator: closure captures its defining environment")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local x = 10; local f = function() return x end; return f()")) == 10);
}

TEST_CASE("evaluator: closure sees updates to captured variables")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local x = 1; local f = function() return x end; x = 99; return f()")) == 99);
}

TEST_CASE("evaluator: closure as updatable counter (the classic)")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local function make() local n = 0; return function() n = n + 1; return n end end; "
        "local c = make(); c(); c(); return c()")) == 3);
}

TEST_CASE("evaluator: recursion via local function")
{
    EvalRig g;
    // Factorial.
    CHECK(as_int(g.run_scalar(
        "local function fact(n) if n <= 1 then return 1 end; return n * fact(n - 1) end; "
        "return fact(5)")) == 120);
    // Fibonacci.
    CHECK(as_int(g.run_scalar(
        "local function fib(n) if n < 2 then return n end; return fib(n-1) + fib(n-2) end; "
        "return fib(10)")) == 55);
}

TEST_CASE("evaluator: recursion depth guard prevents stack overflow")
{
    EvalRig g;
    bool threw = false;
    try {
        (void)g.run_scalar("local function f() return f() end; return f()");
    } catch (const LuaError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("stack overflow") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("evaluator: local function statement")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local function f(x) return x + 1 end; return f(41)")) == 42);
}

TEST_CASE("evaluator: function statement (global)")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("function f(x) return x * 3 end; return f(14)")) == 42);
}

TEST_CASE("evaluator: calling a non-function errors")
{
    EvalRig g;
    bool threw = false;
    try { (void)g.run_scalar("local x = 5; return x()"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("evaluator: multires — last call expands, non-last truncates")
{
    EvalRig g;
    auto v = g.run("local f = function() return 1, 2, 3 end; return f(), 99");
    REQUIRE(v.size() == 2);   // f() in non-last position -> 1 value; 99 last
    CHECK(as_int(v[0]) == 1);
    CHECK(as_int(v[1]) == 99);
    // As the LAST expr, f() expands fully.
    auto v2 = g.run("local f = function() return 1, 2, 3 end; return 0, f()");
    REQUIRE(v2.size() == 4);
    CHECK(as_int(v2[3]) == 3);
}

TEST_CASE("evaluator: Paren truncates multires even in last position")
{
    EvalRig g;
    auto v = g.run("local f = function() return 1, 2, 3 end; return (f())");
    REQUIRE(v.size() == 1);   // parens force single value
    CHECK(as_int(v[0]) == 1);
}

TEST_CASE("evaluator: call statement discards results")
{
    EvalRig g;
    // A call as a statement runs for side effects; here via a counter.
    CHECK(as_int(g.run_scalar(
        "local n = 0; local function bump() n = n + 1 end; bump(); bump(); return n")) == 2);
}

// =====================================================================
// Step 6: tables
// =====================================================================

TEST_CASE("evaluator: empty table")
{
    EvalRig g;
    LuaValue v = g.run_scalar("return {}");
    CHECK(v.is_table());
}

TEST_CASE("evaluator: positional table constructor")
{
    EvalRig g;
    // Verify positional entries via index reads in Lua itself.
    CHECK(as_int(g.run_scalar("local t = {10, 20, 30}; return t[1]")) == 10);
    CHECK(as_int(g.run_scalar("local t = {10, 20, 30}; return t[2]")) == 20);
    CHECK(as_int(g.run_scalar("local t = {10, 20, 30}; return t[3]")) == 30);
    CHECK(g.run_scalar("local t = {10, 20, 30}; return t[4]").is_nil());
}

TEST_CASE("evaluator: table index get/set via [] and .")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local t = {}; t[1] = 99; return t[1]")) == 99);
    CHECK(as_int(g.run_scalar("local t = {}; t.x = 7; return t.x")) == 7);
    CHECK(g.run_scalar("local t = {}; return t.missing").is_nil());   // missing -> nil
}

TEST_CASE("evaluator: named and bracketed fields in constructor")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local t = {x = 1, y = 2}; return t.x + t.y")) == 3);
    CHECK(as_int(g.run_scalar("local t = {x = 5}; return t[\"x\"]")) == 5);   // [str]
    CHECK(as_int(g.run_scalar("local k = \"k\"; local t = {[k] = 8}; return t.k")) == 8);
}

TEST_CASE("evaluator: integer and float keys collapse (t[1] == t[1.0])")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local t = {}; t[1] = 42; return t[1.0]")) == 42);
    CHECK(as_int(g.run_scalar("local t = {}; t[1.0] = 7; return t[1]")) == 7);
}

TEST_CASE("evaluator: length (#) on a sequence table")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return #{1, 2, 3, 4}")) == 4);
    CHECK(as_int(g.run_scalar("local t = {}; t[1] = 1; t[2] = 2; return #t")) == 2);
    CHECK(as_int(g.run_scalar("return #{}")) == 0);
}

TEST_CASE("evaluator: tables are by reference (aliasing)")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local a = {}; local b = a; b.x = 5; return a.x")) == 5);
}

TEST_CASE("evaluator: nested tables")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local t = {a = {b = {c = 42}}}; return t.a.b.c")) == 42);
}

TEST_CASE("evaluator: constructor with trailing call expands multires")
{
    EvalRig g;
    // The last positional field is a call returning 3 values -> all become
    // positional entries 1, 2, 3.
    CHECK(as_int(g.run_scalar(
        "local f = function() return 10, 20, 30 end; local t = {f()}; return #t")) == 3);
}

TEST_CASE("evaluator: assigning nil erases a table entry")
{
    EvalRig g;
    CHECK(g.run_scalar("local t = {x = 1}; t.x = nil; return t.x").is_nil());
    CHECK(as_int(g.run_scalar("local t = {1, 2, 3}; t[2] = nil; return #t")) == 1);
}

TEST_CASE("evaluator: method call (obj:method())")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local t = {value = 42}; function t:get() return self.value end; return t:get()")) == 42);
}

// =====================================================================
// Step 7: builtins
// =====================================================================

TEST_CASE("evaluator: print outputs tab-separated values")
{
    EvalRig g;
    g.run("print(1, \"hello\", true, nil)");
    CHECK(g.out.str() == "1\thello\ttrue\tnil\n");
}

TEST_CASE("evaluator: print of a float shows .0")
{
    EvalRig g;
    g.run("print(3.0)");
    CHECK(g.out.str() == "3.0\n");
}

TEST_CASE("evaluator: type()")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return type(1)")) == "number");
    CHECK(as_str(g.run_scalar("return type(\"x\")")) == "string");
    CHECK(as_str(g.run_scalar("return type(nil)")) == "nil");
    CHECK(as_str(g.run_scalar("return type(true)")) == "boolean");
    CHECK(as_str(g.run_scalar("return type({})")) == "table");
    CHECK(as_str(g.run_scalar("return type(print)")) == "function");
}

TEST_CASE("evaluator: tostring()")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return tostring(42)")) == "42");
    CHECK(as_str(g.run_scalar("return tostring(nil)")) == "nil");
    CHECK(as_str(g.run_scalar("return tostring(false)")) == "false");
    CHECK(as_str(g.run_scalar("return tostring(\"x\")")) == "x");
}

TEST_CASE("evaluator: tonumber()")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return tonumber(\"42\")")) == 42);
    CHECK(as_flt(g.run_scalar("return tonumber(\"3.14\")")) == 3.14);
    CHECK(g.run_scalar("return tonumber(\"abc\")").is_nil());   // unparseable
    CHECK(as_int(g.run_scalar("return tonumber(7)")) == 7);     // number passthrough
}

TEST_CASE("evaluator: error() throws")
{
    EvalRig g;
    bool threw = false;
    try { (void)g.run_scalar("error(\"boom\")"); }
    catch (const LuaError& e) {
        threw = true;
        CHECK(std::string(e.what()) == "boom");
    }
    CHECK(threw);
}

TEST_CASE("evaluator: assert() passes through or throws")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return assert(42)")) == 42);   // truthy -> returns it
    bool threw = false;
    try { (void)g.run_scalar("assert(nil, \"nope\")"); }
    catch (const LuaError& e) {
        threw = true;
        CHECK(std::string(e.what()) == "nope");
    }
    CHECK(threw);
}

TEST_CASE("evaluator: ipairs() iterates 1..n")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local t = {10, 20, 30}; local s = 0; "
        "for i, v in ipairs(t) do s = s + v end; return s")) == 60);
}

TEST_CASE("evaluator: pairs() iterates all keys")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local t = {a = 1, b = 2, c = 3}; local n = 0; "
        "for k, v in pairs(t) do n = n + 1 end; return n")) == 3);
}

TEST_CASE("evaluator: select()")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return select(\"#\", 1, 2, 3)")) == 3);
    auto v = g.run("return select(2, \"a\", \"b\", \"c\")");
    REQUIRE(v.size() == 2);
    CHECK(as_str(v[0]) == "b");
    CHECK(as_str(v[1]) == "c");
}

TEST_CASE("evaluator: rawget / rawset / rawequal / rawlen")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("local t = {}; rawset(t, \"x\", 9); return rawget(t, \"x\")")) == 9);
    CHECK(as_bool(g.run_scalar("return rawequal(1, 1)")) == true);
    CHECK(as_bool(g.run_scalar("return rawequal(1, \"1\")")) == false);
    CHECK(as_int(g.run_scalar("return rawlen({1, 2, 3, 4})")) == 4);
    CHECK(as_int(g.run_scalar("return rawlen(\"hello\")")) == 5);
}

TEST_CASE("evaluator: generic-for with a multi-return iterator")
{
    EvalRig g;
    // A custom iterator function returning successive values.
    CHECK(as_int(g.run_scalar(
        "local function gen(n) local i = 0; "
        "return function() i = i + 1; if i <= n then return i end end end; "
        "local s = 0; for v in gen(5) do s = s + v end; return s")) == 15);
}

TEST_CASE("evaluator: pairs with multiple assignment loop vars")
{
    EvalRig g;
    // Collect keys into a counter to verify both k and v bind.
    CHECK(as_int(g.run_scalar(
        "local t = {x = 10, y = 20}; local sumv = 0; "
        "for k, v in pairs(t) do sumv = sumv + v end; return sumv")) == 30);
}

// =====================================================================
// M2.1: metatables + metamethods
// =====================================================================

TEST_CASE("evaluator: setmetatable / getmetatable basics")
{
    EvalRig g;
    // Without a metatable, getmetatable returns nil.
    CHECK(as_bool(g.run_scalar(
        "local t = {}; return getmetatable(t) == nil")) == true);
    // Attach + round-trip identity.
    CHECK(as_bool(g.run_scalar(
        "local t = {}; local mt = {}; setmetatable(t, mt); "
        "return getmetatable(t) == mt")) == true);
    // setmetatable returns the table (chainable).
    CHECK(as_bool(g.run_scalar(
        "local mt = {}; local t = setmetatable({}, mt); "
        "return getmetatable(t) == mt")) == true);
    // nil clears the metatable.
    CHECK(as_bool(g.run_scalar(
        "local t = setmetatable({}, {}); setmetatable(t, nil); "
        "return getmetatable(t) == nil")) == true);
    // Non-table arg errors.
    bool threw = false;
    try { (void)g.run_scalar("setmetatable(5, {})"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
    // Non-table metatable errors.
    threw = false;
    try { (void)g.run_scalar("setmetatable({}, 5)"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("evaluator: arithmetic metamethods (7 events)")
{
    EvalRig g;
    const char* setup =
        "local mt = { "
        "  __add  = function(a, b) return 'add' end, "
        "  __sub  = function(a, b) return 'sub' end, "
        "  __mul  = function(a, b) return 'mul' end, "
        "  __div  = function(a, b) return 'div' end, "
        "  __idiv = function(a, b) return 'idiv' end, "
        "  __mod  = function(a, b) return 'mod' end, "
        "  __pow  = function(a, b) return 'pow' end } ";
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t + 1")) == "add");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return 1 + t")) == "add");  // right operand
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t - 1")) == "sub");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t * 1")) == "mul");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t / 1")) == "div");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t // 1")) == "idiv");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t % 1")) == "mod");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t ^ 1")) == "pow");
    // Left operand's metamethod wins when BOTH operands have one.
    CHECK(as_str(g.run_scalar(
        "local a = setmetatable({}, {__add = function() return 'A' end}); "
        "local b = setmetatable({}, {__add = function() return 'B' end}); "
        "return a + b")) == "A");
    // Raw numbers bypass entirely.
    CHECK(as_int(g.run_scalar("return 2 + 3")) == 5);
    // No metamethod and not numbers -> error.
    bool threw = false;
    try { (void)g.run_scalar("local t = {}; return t + 1"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("evaluator: unary - and # metamethods")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local t = setmetatable({}, {__unm = function(a) return 'neg' end}); "
        "return -t")) == "neg");
    CHECK(as_int(g.run_scalar(
        "local t = setmetatable({}, {__len = function(a) return 42 end}); "
        "return #t")) == 42);
    // # still works on raw tables/strings.
    CHECK(as_int(g.run_scalar("return #{1,2,3}")) == 3);
    CHECK(as_int(g.run_scalar("return #\"hi\"")) == 2);
}

TEST_CASE("evaluator: bitwise metamethods")
{
    EvalRig g;
    const char* setup =
        "local mt = { "
        "  __band = function(a, b) return 'band' end, "
        "  __bor  = function(a, b) return 'bor' end, "
        "  __bxor = function(a, b) return 'bxor' end, "
        "  __shl  = function(a, b) return 'shl' end, "
        "  __shr  = function(a, b) return 'shr' end, "
        "  __bnot = function(a) return 'bnot' end } ";
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t & 1")) == "band");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t | 1")) == "bor");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t ~ 1")) == "bxor");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t << 1")) == "shl");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return t >> 1")) == "shr");
    CHECK(as_str(g.run_scalar(std::string{setup} +
        "local t = setmetatable({}, mt); return ~t")) == "bnot");
    // Raw ints bypass.
    CHECK(as_int(g.run_scalar("return 6 & 3")) == 2);
}

TEST_CASE("evaluator: __concat metamethod")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local t = setmetatable({}, {__concat = function(a, b) return 'cc' end}); "
        "return t .. 1")) == "cc");
    // Right operand's metamethod when only it has one.
    CHECK(as_str(g.run_scalar(
        "local t = setmetatable({}, {__concat = function(a, b) return 'cc' end}); "
        "return 'x' .. t")) == "cc");
    // Raw concat still works.
    CHECK(as_str(g.run_scalar("return 'a' .. 'b' .. 1")) == "ab1");
}

TEST_CASE("evaluator: __eq / __lt / __le metamethods + derived")
{
    EvalRig g;
    // __eq only fires for distinct tables not raw-equal.
    CHECK(as_bool(g.run_scalar(
        "local mt = {__eq = function(a, b) return true end}; "
        "local a = setmetatable({}, mt); local b = setmetatable({}, mt); "
        "return a == b")) == true);
    CHECK(as_bool(g.run_scalar(
        "local mt = {__eq = function(a, b) return true end}; "
        "local a = setmetatable({}, mt); local b = setmetatable({}, mt); "
        "return a ~= b")) == false);
    // Different types never use __eq.
    CHECK(as_bool(g.run_scalar(
        "local a = setmetatable({}, {__eq = function() return true end}); "
        "return a == 5")) == false);
    // Raw-equal objects skip __eq.
    CHECK(as_bool(g.run_scalar("return 1 == 1.0")) == true);

    // __lt / __le on tables.
    CHECK(as_bool(g.run_scalar(
        "local mt = {__lt = function(a, b) return true end}; "
        "local a = setmetatable({}, mt); local b = setmetatable({}, mt); "
        "return a < b")) == true);
    CHECK(as_bool(g.run_scalar(
        "local mt = {__le = function(a, b) return true end}; "
        "local a = setmetatable({}, mt); local b = setmetatable({}, mt); "
        "return a <= b")) == true);
    // Derived: a > b uses __lt(b, a); here always true -> a > b is true.
    CHECK(as_bool(g.run_scalar(
        "local mt = {__lt = function(a, b) return true end}; "
        "local a = setmetatable({}, mt); local b = setmetatable({}, mt); "
        "return a > b")) == true);
    // __le fallback to `not (b < a)` when only __lt present.
    CHECK(as_bool(g.run_scalar(
        "local mt = {__lt = function(a, b) return false end}; "
        "local a = setmetatable({}, mt); local b = setmetatable({}, mt); "
        "return a <= b")) == true);   // not (b < a) = not false = true
    // Raw ordering still works.
    CHECK(as_bool(g.run_scalar("return 1 < 2 and 'a' < 'b'")) == true);
}

TEST_CASE("evaluator: __index as a table (prototype OOP)")
{
    EvalRig g;
    // Prototype lookup: missing key falls through to __index table.
    CHECK(as_bool(g.run_scalar(
        "local proto = {kind = 'thing'}; "
        "local obj = setmetatable({}, {__index = proto}); "
        "return obj.kind == 'thing' and "   // string compare -> true
        "       obj.missing == nil")) == true);  // absent -> nil
    // Multi-level prototype chain.
    CHECK(as_str(g.run_scalar(
        "local gp = {v = 'deep'}; "
        "local p = setmetatable({}, {__index = gp}); "
        "local obj = setmetatable({}, {__index = p}); "
        "return obj.v")) == "deep");
    // Method via prototype.
    CHECK(as_str(g.run_scalar(
        "local proto = {greet = function(self) return 'hi' end}; "
        "local obj = setmetatable({}, {__index = proto}); "
        "return obj:greet()")) == "hi");
}

TEST_CASE("evaluator: __index as a function")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local obj = setmetatable({}, "
        "  {__index = function(t, k) return 'default:' .. k end}); "
        "return obj.foo")) == "default:foo");
}

TEST_CASE("evaluator: raw slot wins over __index / __newindex")
{
    EvalRig g;
    // __index: a present non-nil slot is returned, __index not consulted.
    CHECK(as_int(g.run_scalar(
        "local obj = setmetatable({n = 7}, "
        "  {__index = function() return 999 end}); "
        "return obj.n")) == 7);
    // __newindex: an existing non-nil slot is overwritten directly; the
    // metamethod must NOT fire. We make __newindex error to prove it.
    CHECK(as_int(g.run_scalar(
        "local t = setmetatable({x = 1}, "
        "  {__newindex = function() error('nope') end}); "
        "t.x = 5; return t.x")) == 5);
    // __newindex function IS called for an absent slot (value not stored raw).
    CHECK(as_bool(g.run_scalar(
        "local store = {}; "
        "local t = setmetatable({}, {__newindex = function(tab, k, v) "
        "  store[k] = v end}); "
        "t.k = 'v'; return store.k == 'v' and t.k == nil")) == true);
    // __newindex as a table: writes go to the shadow table.
    CHECK(as_str(g.run_scalar(
        "local shadow = {}; "
        "local t = setmetatable({}, {__newindex = shadow}); "
        "t.k = 'v'; return shadow.k")) == "v");
}

TEST_CASE("evaluator: __call metamethod")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local t = setmetatable({}, "
        "  {__call = function(self, ...) return select('#', ...) end}); "
        "return t(1, 2, 3)")) == 3);
    // The callable value is passed as the first argument.
    CHECK(as_int(g.run_scalar(
        "local t = setmetatable({n = 7}, "
        "  {__call = function(self) return self.n end}); "
        "return t()")) == 7);
    // Calling a non-callable without __call errors.
    bool threw = false;
    try { (void)g.run_scalar("local t = {}; return t()"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("evaluator: __tostring metamethod via tostring()/print()")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local t = setmetatable({}, {__tostring = function() return 'T!' end}); "
        "return tostring(t)")) == "T!");
    // print routes each arg through tostring (hence __tostring).
    g.run("local t = setmetatable({}, {__tostring = function() return 'OBJ' end}); "
          "print(t)");
    CHECK(g.out.str() == "OBJ\n");
    // __tostring must return a string.
    bool threw = false;
    try { (void)g.run_scalar(
        "local t = setmetatable({}, {__tostring = function() return 5 end}); "
        "return tostring(t)"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
    // No __tostring -> raw rendering ("table: 0x...").
    CHECK(g.run_scalar("local t = {}; return tostring(t)").as_str()->data.substr(0, 5)
          == "table");
}

TEST_CASE("evaluator: full OOP class pattern (Vector)")
{
    EvalRig g;
    const char* cls =
        "local Vector = {} Vector.__index = Vector "
        "function Vector.new(x, y) return setmetatable({x = x, y = y}, Vector) end "
        "function Vector:len() return (self.x * self.x + self.y * self.y) ^ 0.5 end "
        "function Vector.__add(a, b) return Vector.new(a.x + b.x, a.y + b.y) end "
        "function Vector.__eq(a, b) return a.x == b.x and a.y == b.y end ";
    // len() on (3,4) -> 5.0.
    CHECK(as_flt(g.run_scalar(std::string{cls} +
        "return Vector.new(3,4):len()")) == 5.0);
    // operator overloading: (3,4)+(1,1)=(4,5) -> len ~6.4031.
    CHECK(as_flt(g.run_scalar(std::string{cls} +
        "return (Vector.new(3,4) + Vector.new(1,1)):len()")) == doctest::Approx(6.4031));
    // __eq value equality.
    CHECK(as_bool(g.run_scalar(std::string{cls} +
        "return Vector.new(3,4) == Vector.new(3,4)")) == true);
    CHECK(as_bool(g.run_scalar(std::string{cls} +
        "return Vector.new(3,4) == Vector.new(3,5)")) == false);
}

TEST_CASE("evaluator: raw* builtins bypass metamethods after M2.1")
{
    EvalRig g;
    // rawget ignores __index.
    CHECK(as_bool(g.run_scalar(
        "local t = setmetatable({}, {__index = function() return 'X' end}); "
        "return rawget(t, 'k') == nil")) == true);
    // rawset ignores __newindex.
    CHECK(as_int(g.run_scalar(
        "local t = setmetatable({}, {__newindex = function() error('nope') end}); "
        "rawset(t, 'k', 9); return rawget(t, 'k')")) == 9);
    // rawequal ignores __eq.
    CHECK(as_bool(g.run_scalar(
        "local mt = {__eq = function() return true end}; "
        "local a = setmetatable({}, mt); local b = setmetatable({}, mt); "
        "return rawequal(a, b)")) == false);
    // rawlen ignores __len.
    CHECK(as_int(g.run_scalar(
        "local t = setmetatable({}, {__len = function() return 999 end}); "
        "return rawlen(t)")) == 0);
}

// =====================================================================
// M2.2: goto / labels
// =====================================================================

TEST_CASE("evaluator: backward goto (loop pattern)")
{
    EvalRig g;
    // A backward goto implements a while-loop: count 1..5.
    CHECK(as_int(g.run_scalar(
        "local i = 1; local s = 0; "
        "::top:: if i <= 5 then s = s + i; i = i + 1; goto top end; "
        "return s")) == 15);
}

TEST_CASE("evaluator: forward goto (skip then continue)")
{
    EvalRig g;
    // Skip over an assignment to x, then the next statement runs.
    CHECK(as_int(g.run_scalar(
        "local x = 1; goto skip; x = 999; ::skip:: x = x + 10; return x")) == 11);
}

TEST_CASE("evaluator: mutual goto state machine (l1 -> l2 -> l3)")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local log = {} "
        "::l1:: log[#log+1] = 1; goto l2; "
        "::l2:: log[#log+1] = 2; goto l3; "
        "::l3:: log[#log+1] = 3; "
        "local s = 0; for i = 1, #log do s = s + log[i] end; "
        "return s")) == 6);
}

TEST_CASE("evaluator: goto out of nested do block")
{
    EvalRig g;
    // Label is in the enclosing block; goto propagates through the Do. Execution
    // resumes AT the label and continues (the `x = x + 100` between is skipped).
    CHECK(as_int(g.run_scalar(
        "local x = 0; "
        "do x = x + 1; goto done; x = 999 end; "   // x = 1, goto escapes the do
        "x = x + 1000; "                            // SKIPPED
        "::done:: x = x + 100; return x")) == 101); // resumes here: x = 1 + 100
}

TEST_CASE("evaluator: goto out of while / repeat / numeric-for / generic-for")
{
    EvalRig g;
    // while
    CHECK(as_int(g.run_scalar(
        "local i = 0; local hit = 0; "
        "while true do i = i + 1; if i == 3 then hit = i; goto out end end; "
        "::out:: return hit")) == 3);
    // repeat-until
    CHECK(as_int(g.run_scalar(
        "local i = 0; local hit = 0; "
        "repeat i = i + 1; if i == 4 then hit = i; goto out end until false; "
        "::out:: return hit")) == 4);
    // numeric for
    CHECK(as_int(g.run_scalar(
        "local hit = 0; "
        "for i = 1, 100 do if i == 7 then hit = i; goto out end end; "
        "::out:: return hit")) == 7);
    // generic for (ipairs)
    CHECK(as_int(g.run_scalar(
        "local hit = 0; "
        "for i, v in ipairs({10,20,30,40}) do "
        "  if v == 30 then hit = i; goto out end "
        "end; "
        "::out:: return hit")) == 3);
}

TEST_CASE("evaluator: goto inside if branch to enclosing label")
{
    EvalRig g;
    // The label lives in the block enclosing the if; the goto is inside an if
    // branch. Both Do/If propagate Goto via `return eval_block_box(...)`.
    CHECK(as_int(g.run_scalar(
        "local x = 0; "
        "if true then x = x + 1; goto done; x = x + 999 end; "
        "::done:: x = x + 10; return x")) == 11);
}

TEST_CASE("evaluator: multiple distinct labels in one block")
{
    EvalRig g;
    // Two labels; a goto must pick the named one, not the nearest. (Lua
    // requires `return` to be the last statement in a block, so each branch's
    // return is wrapped in its own do-end block.)
    CHECK(as_str(g.run_scalar(
        "goto b; "
        "::a:: do return 'A' end; "
        "::b:: do return 'B' end")) == "B");
}

TEST_CASE("evaluator: no visible label raises LuaError")
{
    EvalRig g;
    bool threw = false;
    try { (void)g.run_scalar("goto missing"); }
    catch (const LuaError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("no visible label") != std::string::npos);
        CHECK(std::string(e.what()).find("'missing'") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("evaluator: cross-function goto raises when function called")
{
    EvalRig g;
    // A goto whose label is in the enclosing chunk: inside the function it has
    // no visible label, so calling f() must raise.
    bool threw = false;
    try {
        (void)g.run_scalar(
            "local function f() goto escape end; "
            "f(); "
            "::escape:: return 1");
    }
    catch (const LuaError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("no visible label") != std::string::npos);
        CHECK(std::string(e.what()).find("'escape'") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("evaluator: goto label is case-sensitive")
{
    EvalRig g;
    // Lua labels are case-sensitive identifiers; ::Loop:: != ::loop::.
    bool threw = false;
    try { (void)g.run_scalar("goto Loop; ::loop::"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("evaluator: goto across repeated loop iterations works")
{
    EvalRig g;
    // Re-entering a loop body block reuses the cached label map: a goto inside
    // the loop body whose label is also inside the body must work on every
    // iteration, not just the first. (Exercises the memoization path.)
    CHECK(as_int(g.run_scalar(
        "local s = 0; "
        "for i = 1, 3 do "
        "  goto body; "
        "  s = s + 1000; "        // skipped each iteration
        "  ::body:: s = s + i "   // runs each iteration
        "end; "
        "return s")) == 6);       // 1 + 2 + 3
}

// =====================================================================
// M2.3: pcall / xpcall / error objects
// =====================================================================

TEST_CASE("evaluator: pcall success returns true + results")
{
    EvalRig g;
    // No args -> true, no extra values.
    {
        auto v = g.run("return pcall(function() end)");
        REQUIRE(v.size() == 1);
        CHECK(as_bool(v[0]) == true);
    }
    // Single return.
    {
        auto v = g.run("return pcall(function() return 42 end)");
        REQUIRE(v.size() == 2);
        CHECK(as_bool(v[0]) == true);
        CHECK(as_int(v[1]) == 42);
    }
    // Multi return.
    {
        auto v = g.run("return pcall(function() return 1, 2, 3 end)");
        REQUIRE(v.size() == 4);
        CHECK(as_bool(v[0]) == true);
        CHECK(as_int(v[3]) == 3);
    }
    // Args forwarded.
    {
        auto v = g.run("return pcall(function(a, b) return a + b end, 10, 20)");
        REQUIRE(v.size() == 2);
        CHECK(as_bool(v[0]) == true);
        CHECK(as_int(v[1]) == 30);
    }
}

TEST_CASE("evaluator: pcall catches error(string)")
{
    EvalRig g;
    auto v = g.run("return pcall(function() error('boom') end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_str(v[1]) == "boom");
}

TEST_CASE("evaluator: pcall catches error(table) — returns the object")
{
    EvalRig g;
    auto v = g.run("return pcall(function() error({code = 42}) end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    REQUIRE(v[1].is_table());
    // Verify it's the same table (identity, not a copy).
    LuaKey k; k.k = LuaKey::K::Str; k.s = "code";
    auto it = v[1].as_table()->hash.find(k);
    REQUIRE(it != v[1].as_table()->hash.end());
    CHECK(as_int(it->second) == 42);
}

TEST_CASE("evaluator: pcall catches error(number)")
{
    EvalRig g;
    auto v = g.run("return pcall(function() error(99) end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_int(v[1]) == 99);
}

TEST_CASE("evaluator: pcall catches runtime type errors")
{
    EvalRig g;
    // Arithmetic on non-number -> runtime error, message is a string.
    auto v = g.run("return pcall(function() return nil + 1 end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(v[1].is_str());
}

TEST_CASE("evaluator: pcall catches nested errors")
{
    EvalRig g;
    auto v = g.run(
        "return pcall(function() "
        "  local function inner() error('deep') end; "
        "  inner() "
        "end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_str(v[1]) == "deep");
}

TEST_CASE("evaluator: pcall error propagates through pcall boundary")
{
    EvalRig g;
    // An uncaught error still aborts the chunk if pcall isn't used.
    bool threw = false;
    try { (void)g.run_scalar("error('uncaught')"); }
    catch (const LuaError&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("evaluator: error(msg, 0) — level 0, no position prepending")
{
    EvalRig g;
    auto v = g.run("return pcall(function() error('plain', 0) end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_str(v[1]) == "plain");
}

TEST_CASE("evaluator: xpcall success")
{
    EvalRig g;
    auto v = g.run(
        "return xpcall(function() return 'ok' end, "
        "  function(e) return 'handler:' .. e end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == true);
    CHECK(as_str(v[1]) == "ok");
}

TEST_CASE("evaluator: xpcall invokes handler on error")
{
    EvalRig g;
    auto v = g.run(
        "return xpcall(function() error('bang') end, "
        "  function(e) return 'handler:' .. e end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_str(v[1]) == "handler:bang");
}

TEST_CASE("evaluator: xpcall handler receives non-string error object")
{
    EvalRig g;
    auto v = g.run(
        "return xpcall(function() error({val = 7}) end, "
        "  function(e) return e.val end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_int(v[1]) == 7);
}

TEST_CASE("evaluator: xpcall with extra args forwards to f")
{
    EvalRig g;
    auto v = g.run(
        "return xpcall(function(a, b) return a * b end, "
        "  function(e) return e end, 3, 4)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == true);
    CHECK(as_int(v[1]) == 12);
}

TEST_CASE("evaluator: xpcall handler error propagates")
{
    EvalRig g;
    // If the handler itself errors, xpcall does NOT catch it.
    bool threw = false;
    try {
        (void)g.run_scalar(
            "xpcall(function() error('first') end, "
            "  function(e) error('second') end)");
    }
    catch (const LuaError& e) {
        threw = true;
        CHECK(std::string(e.what()) == "second");
    }
    CHECK(threw);
}

TEST_CASE("evaluator: assert with pcall returns message")
{
    EvalRig g;
    auto v = g.run("return pcall(function() assert(nil, 'nope') end)");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_str(v[1]) == "nope");
}

TEST_CASE("evaluator: pcall can be nested")
{
    EvalRig g;
    auto v = g.run(
        "local ok, err = pcall(function() "
        "  pcall(function() error('inner') end); "
        "  error('outer') "
        "end); "
        "return ok, err");
    REQUIRE(v.size() == 2);
    CHECK(as_bool(v[0]) == false);
    CHECK(as_str(v[1]) == "outer");
}

// =====================================================================
// M3.1: string library
// =====================================================================

TEST_CASE("evaluator: string.len / string.sub")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return string.len('hello')")) == 5);
    CHECK(as_str(g.run_scalar("return string.sub('hello', 2, 4)")) == "ell");
    CHECK(as_str(g.run_scalar("return string.sub('hello', -2)")) == "lo");
    CHECK(as_str(g.run_scalar("return string.sub('hello', 2)")) == "ello");
    CHECK(as_str(g.run_scalar("return string.sub('hello', -2, -1)")) == "lo");
}

TEST_CASE("evaluator: string.upper / string.lower")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return string.upper('Hi')")) == "HI");
    CHECK(as_str(g.run_scalar("return string.lower('Hi')")) == "hi");
}

TEST_CASE("evaluator: string.rep / string.reverse")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return string.rep('ab', 3)")) == "ababab");
    CHECK(as_str(g.run_scalar("return string.rep('x', 3, '-')")) == "x-x-x");
    CHECK(as_str(g.run_scalar("return string.rep('ab', 0)")) == "");
    CHECK(as_str(g.run_scalar("return string.reverse('abc')")) == "cba");
}

TEST_CASE("evaluator: string.byte / string.char")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return string.byte('A')")) == 65);
    CHECK(as_str(g.run_scalar("return string.char(72, 105)")) == "Hi");
}

TEST_CASE("evaluator: per-type string metatable — method call syntax")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return ('hello'):upper()")) == "HELLO");
    CHECK(as_int(g.run_scalar("return ('hello'):len()")) == 5);
    CHECK(as_str(g.run_scalar("return ('hello'):sub(2, 4)")) == "ell");
}

TEST_CASE("evaluator: string.format basics")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return string.format('%d', 42)")) == "42");
    CHECK(as_str(g.run_scalar("return string.format('%05d', 42)")) == "00042");
    CHECK(as_str(g.run_scalar("return string.format('%x', 255)")) == "ff");
    CHECK(as_str(g.run_scalar("return string.format('%f', 3.14)")) == "3.140000");
    CHECK(as_str(g.run_scalar("return string.format('%.2f', 3.14159)")) == "3.14");
    CHECK(as_str(g.run_scalar("return string.format('%s', 'hi')")) == "hi");
    CHECK(as_str(g.run_scalar("return string.format('%-10s|', 'hi')")) == "hi        |");
    CHECK(as_str(g.run_scalar("return string.format('%c', 65)")) == "A");
    CHECK(as_str(g.run_scalar("return string.format('%%d', 42)")) == "%d");
}

TEST_CASE("evaluator: string.find")
{
    EvalRig g;
    {
        auto v = g.run("return string.find('hello world', 'world')");
        REQUIRE(v.size() == 2);
        CHECK(as_int(v[0]) == 7);
        CHECK(as_int(v[1]) == 11);
    }
    {
        auto v = g.run("return string.find('hello world', '(%w+) (%w+)')");
        REQUIRE(v.size() == 4);
        CHECK(as_int(v[0]) == 1);
        CHECK(as_int(v[1]) == 11);
        CHECK(as_str(v[2]) == "hello");
        CHECK(as_str(v[3]) == "world");
    }
    CHECK(g.run_scalar("return string.find('hello', 'xyz')").is_nil() == true);
}

TEST_CASE("evaluator: string.match")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return string.match('2024-01-15', '(%d+)-(%d+)-(%d+)')")) == "2024");
    {
        auto v = g.run("return string.match('2024-01-15', '(%d+)-(%d+)-(%d+)')");
        REQUIRE(v.size() == 3);
        CHECK(as_str(v[0]) == "2024");
        CHECK(as_str(v[1]) == "01");
        CHECK(as_str(v[2]) == "15");
    }
}

TEST_CASE("evaluator: string.gsub")
{
    EvalRig g;
    {
        auto v = g.run("return string.gsub('hello world', 'o', '0')");
        REQUIRE(v.size() == 2);
        CHECK(as_str(v[0]) == "hell0 w0rld");
        CHECK(as_int(v[1]) == 2);
    }
    // Replacement with %0 (whole match)
    CHECK(as_str(g.run_scalar("return string.gsub('hi', '(.)', '%1%1')")) == "hhii");
    // Function replacement
    CHECK(as_str(g.run_scalar(
        "return string.gsub('hello', '.', function(c) return c:upper() end)")) == "HELLO");
    // Table replacement with __index
    CHECK(as_str(g.run_scalar(
        "local t = setmetatable({}, {__index = function(t, k) return k:upper() end}); "
        "return string.gsub('a alo b', '%w%w+', t)")) == "a ALO b");
    // 5.3.3 empty-match rule
    CHECK(as_str(g.run_scalar("return string.gsub('a b cd', ' *', '-')")) == "-a-b-c-d-");
    // nil return from function: keep original
    CHECK(as_str(g.run_scalar(
        "return string.gsub('hello', '.', function() return nil end)")) == "hello");
}

TEST_CASE("evaluator: string.gmatch")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar(
        "local s = 0; for w in string.gmatch('1,2,3,4', '[^,]+') do s = s + tonumber(w) end; "
        "return s")) == 10);
}

TEST_CASE("evaluator: pattern %b balanced")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return string.match('(a(b)c)', '%b()')")) == "(a(b)c)");
}

TEST_CASE("evaluator: string interning — short string identity")
{
    EvalRig g;
    // Two equal short strings should be the same object (interned).
    // This is visible via == (which works on string content) and indirectly
    // through format %p (though %p is hard to assert in unit tests).
    CHECK(as_bool(g.run_scalar("local a = 'ab'; local b = 'ab'; return a == b")) == true);
}

// =====================================================================
// M3.2: table library
// =====================================================================

TEST_CASE("evaluator: table.insert — append form")
{
    EvalRig g;
    g.run("local t = {}; table.insert(t, 10); table.insert(t, 20); "
          "table.insert(t, 30)");
    CHECK(as_int(g.run_scalar("local t = {10,20,30}; return #t")) == 3);
    // verify insert actually appends in order
    CHECK(as_str(g.run_scalar(
        "local t = {}; "
        "table.insert(t, 'a'); table.insert(t, 'b'); table.insert(t, 'c'); "
        "return t[2]")) == "b");
}

TEST_CASE("evaluator: table.insert — positional form shifts")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local t = {'a','b','d'}; "
        "table.insert(t, 3, 'c'); "
        "return table.concat(t)")) == "abcd");
    CHECK(as_str(g.run_scalar(
        "local t = {'b','c','d'}; "
        "table.insert(t, 1, 'a'); "
        "return table.concat(t)")) == "abcd");
    // insert at position n+1 is equivalent to append
    CHECK(as_str(g.run_scalar(
        "local t = {'a','b','c'}; "
        "table.insert(t, 4, 'd'); "
        "return table.concat(t)")) == "abcd");
}

TEST_CASE("evaluator: table.remove — default + positional")
{
    EvalRig g;
    // Default: remove last
    CHECK(as_str(g.run_scalar(
        "local t = {'a','b','c'}; "
        "local r = table.remove(t); "
        "return r .. '|' .. table.concat(t)")) == "c|ab");
    // Positional: shift down
    CHECK(as_str(g.run_scalar(
        "local t = {'a','b','c'}; "
        "local r = table.remove(t, 1); "
        "return r .. '|' .. table.concat(t)")) == "a|bc");
    // Remove middle
    CHECK(as_str(g.run_scalar(
        "local t = {'a','b','c','d'}; "
        "local r = table.remove(t, 2); "
        "return r .. '|' .. table.concat(t)")) == "b|acd");
    // Remove from empty table returns nothing
    CHECK(g.run("local t = {}; return table.remove(t)").empty());
}

TEST_CASE("evaluator: table.concat — basic + separator")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return table.concat({1,2,3})")) == "123");
    CHECK(as_str(g.run_scalar("return table.concat({1,2,3}, ',')")) == "1,2,3");
    CHECK(as_str(g.run_scalar(
        "return table.concat({'a','b','c'}, '-')")) == "a-b-c");
    // i/j range
    CHECK(as_str(g.run_scalar(
        "return table.concat({1,2,3,4,5}, ',', 2, 4)")) == "2,3,4");
    // Empty range
    CHECK(as_str(g.run_scalar(
        "return table.concat({1,2,3}, ',', 5, 1)")) == "");
    // Numbers (float) render via number_to_string
    CHECK(as_str(g.run_scalar(
        "return table.concat({1.5, 2.5}, '|')")) == "1.5|2.5");
}

TEST_CASE("evaluator: table.concat — error cases")
{
    EvalRig g;
    // Non-string/number element
    auto r1 = g.run(
        "local t = {1, 2, {}}; "
        "local ok, err = pcall(table.concat, t); "
        "return ok, err");
    REQUIRE(r1.size() == 2);
    CHECK(as_bool(r1[0]) == false);
    CHECK(r1[1].as_str()->data.find("invalid value (at index 3)") !=
          std::string::npos);
}

TEST_CASE("evaluator: table.pack / table.unpack round-trip")
{
    EvalRig g;
    // pack preserves arity via .n
    auto r = g.run("return table.pack(10, 20, 30)");
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].is_table());
    // .n is set to the count
    LuaKey nk; nk.k = LuaKey::K::Str; nk.s = "n";
    LuaValue n = r[0].as_table()->hash[nk];
    REQUIRE(n.is_int());
    CHECK(n.as_int() == 3);
    // elements 1..3
    LuaKey k1; k1.k = LuaKey::K::Int; k1.i = 1;
    CHECK(r[0].as_table()->hash[k1].as_int() == 10);

    // unpack round-trips
    CHECK(as_int(g.run_scalar(
        "local t = table.pack(10, 20, 30); "
        "local a, b, c = table.unpack(t); "
        "return a + b + c")) == 60);
    // unpack with range
    CHECK(as_int(g.run_scalar(
        "local t = {10, 20, 30, 40}; "
        "local a, b = table.unpack(t, 2, 3); "
        "return a + b")) == 50);
    // pack captures trailing nils (multires)
    CHECK(as_int(g.run_scalar(
        "local function f() return 1, nil, 3 end; "
        "local t = table.pack(f()); "
        "return t.n")) == 3);
}

TEST_CASE("evaluator: table.move — basic + overlap")
{
    EvalRig g;
    // Basic move within same table: copy elements 1..3 to positions 4..6
    // (the table grows from 5 to 6 elements).
    CHECK(as_str(g.run_scalar(
        "local t = {1,2,3,4,5}; "
        "table.move(t, 1, 3, 4); "
        "return table.concat(t, ',')")) == "1,2,3,1,2,3");
    // Move to a different table
    CHECK(as_str(g.run_scalar(
        "local a = {10,20,30}; local b = {}; "
        "table.move(a, 1, 3, 1, b); "
        "return table.concat(b, ',')")) == "10,20,30");
    // Overlapping forward (dest > source): copy 2..4 to 3..5
    CHECK(as_str(g.run_scalar(
        "local t = {1,2,3,4,5}; "
        "table.move(t, 2, 4, 3); "
        "return table.concat(t, ',')")) == "1,2,2,3,4");
    // Overlapping backward (dest < source): copy 2..4 to 1..3
    CHECK(as_str(g.run_scalar(
        "local t = {1,2,3,4,5}; "
        "table.move(t, 2, 4, 1); "
        "return table.concat(t, ',')")) == "2,3,4,4,5");
    // Returns destination table
    CHECK(as_bool(g.run_scalar(
        "local a = {}; local b = {}; "
        "return table.move(a, 1, 0, 1, b) == b")) == true);
}

TEST_CASE("evaluator: table.sort — numeric default")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local t = {5, 3, 1, 4, 2}; "
        "table.sort(t); "
        "return table.concat(t, ',')")) == "1,2,3,4,5");
    // Reverse with comparator
    CHECK(as_str(g.run_scalar(
        "local t = {5, 3, 1, 4, 2}; "
        "table.sort(t, function(a,b) return a > b end); "
        "return table.concat(t, ',')")) == "5,4,3,2,1");
    // Already sorted
    CHECK(as_str(g.run_scalar(
        "local t = {1,2,3,4,5}; "
        "table.sort(t); "
        "return table.concat(t, ',')")) == "1,2,3,4,5");
    // Reverse-sorted input (worst case for naive quicksort)
    CHECK(as_str(g.run_scalar(
        "local t = {}; "
        "for i = 100, 1, -1 do t[#t+1] = i end; "
        "table.sort(t); "
        "return t[1] .. ',' .. t[100] .. ',' .. t[50]")) == "1,100,50");
}

TEST_CASE("evaluator: table.sort — strings + custom comparator")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local t = {'banana', 'apple', 'cherry'}; "
        "table.sort(t); "
        "return table.concat(t, ',')")) == "apple,banana,cherry");
    // Sort by length
    CHECK(as_str(g.run_scalar(
        "local t = {'aaaa', 'b', 'cc'}; "
        "table.sort(t, function(a,b) return #a < #b end); "
        "return table.concat(t, ',')")) == "b,cc,aaaa");
}

TEST_CASE("evaluator: table.sort — totalness error")
{
    EvalRig g;
    // A comparator that returns true for both orders is non-total; Lua raises.
    auto r = g.run(
        "local t = {1, 2}; "
        "local ok, err = pcall(table.sort, t, function(a,b) return true end); "
        "return ok, tostring(err)");
    REQUIRE(r.size() == 2);
    CHECK(as_bool(r[0]) == false);
    CHECK(r[1].as_str()->data.find("invalid order function") !=
          std::string::npos);
}

TEST_CASE("evaluator: table.sort — empty and singleton")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar(
        "local t = {}; table.sort(t); return table.concat(t)")) == "");
    CHECK(as_int(g.run_scalar(
        "local t = {42}; table.sort(t); return t[1]")) == 42);
}

// =====================================================================
// M3.3: math library
// =====================================================================

TEST_CASE("evaluator: math constants")
{
    EvalRig g;
    CHECK(as_flt(g.run_scalar("return math.pi")) == doctest::Approx(3.14159265358979));
    CHECK(as_flt(g.run_scalar("return math.huge")) ==
          std::numeric_limits<double>::infinity());
    CHECK(as_int(g.run_scalar("return math.maxinteger")) ==
          std::numeric_limits<long long>::max());
    CHECK(as_int(g.run_scalar("return math.mininteger")) ==
          std::numeric_limits<long long>::min());
}

TEST_CASE("evaluator: math.abs subtype preservation")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return math.abs(-5)")) == 5);
    CHECK(as_int(g.run_scalar("return math.abs(5)")) == 5);
    CHECK(as_int(g.run_scalar("return math.abs(0)")) == 0);
    CHECK(as_flt(g.run_scalar("return math.abs(-3.5)")) == 3.5);
    CHECK(as_flt(g.run_scalar("return math.abs(3.5)")) == 3.5);
}

TEST_CASE("evaluator: math.ceil/floor — int stays int, float rounds")
{
    EvalRig g;
    // Ints stay ints
    CHECK(as_int(g.run_scalar("return math.ceil(5)")) == 5);
    CHECK(as_int(g.run_scalar("return math.floor(5)")) == 5);
    // Floats round to integer
    CHECK(as_int(g.run_scalar("return math.ceil(3.2)")) == 4);
    CHECK(as_int(g.run_scalar("return math.floor(3.8)")) == 3);
    CHECK(as_int(g.run_scalar("return math.ceil(-3.2)")) == -3);
    CHECK(as_int(g.run_scalar("return math.floor(-3.8)")) == -4);
    // Out-of-range floats yield nil
    CHECK(g.run_scalar("return math.ceil(math.huge * 1000)").is_nil());
}

TEST_CASE("evaluator: math.exp/log/sqrt/sin/cos/tan")
{
    EvalRig g;
    CHECK(as_flt(g.run_scalar("return math.exp(0)")) == 1.0);
    CHECK(as_flt(g.run_scalar("return math.exp(1)")) ==
          doctest::Approx(2.718281828459045));
    CHECK(as_flt(g.run_scalar("return math.log(math.exp(1))")) ==
          doctest::Approx(1.0));
    CHECK(as_flt(g.run_scalar("return math.log(100, 10)")) ==
          doctest::Approx(2.0));
    CHECK(as_flt(g.run_scalar("return math.sqrt(16)")) == 4.0);
    CHECK(as_flt(g.run_scalar("return math.sqrt(2)")) ==
          doctest::Approx(1.4142135623730951));
    CHECK(as_flt(g.run_scalar("return math.sin(0)")) == 0.0);
    CHECK(as_flt(g.run_scalar("return math.cos(0)")) == 1.0);
    CHECK(as_flt(g.run_scalar("return math.tan(0)")) == 0.0);
    CHECK(as_flt(g.run_scalar("return math.sin(math.pi / 2)")) ==
          doctest::Approx(1.0));
}

TEST_CASE("evaluator: math.fmod subtype preservation")
{
    EvalRig g;
    // int,int -> int
    CHECK(as_int(g.run_scalar("return math.fmod(7, 3)")) == 1);
    CHECK(as_int(g.run_scalar("return math.fmod(-7, 3)")) == -1);
    // float involvement -> float
    CHECK(as_flt(g.run_scalar("return math.fmod(7.5, 2)")) ==
          doctest::Approx(1.5));
    // fmod by 0 with ints: returns NaN (Lua 5.4 behavior)
    CHECK(std::isnan(as_flt(g.run_scalar("return math.fmod(7, 0)"))));
}

TEST_CASE("evaluator: math.modf returns two values")
{
    EvalRig g;
    auto r = g.run("return math.modf(3.75)");
    REQUIRE(r.size() == 2);
    CHECK(as_flt(r[0]) == 3.0);
    CHECK(as_flt(r[1]) == 0.75);
    // Negative
    r = g.run("return math.modf(-2.5)");
    REQUIRE(r.size() == 2);
    CHECK(as_flt(r[0]) == -2.0);
    CHECK(as_flt(r[1]) == -0.5);
    // Already-integral float
    r = g.run("return math.modf(5.0)");
    REQUIRE(r.size() == 2);
    CHECK(as_flt(r[0]) == 5.0);
    CHECK(as_flt(r[1]) == 0.0);
}

TEST_CASE("evaluator: math.pow always float")
{
    EvalRig g;
    CHECK(as_flt(g.run_scalar("return math.pow(2, 10)")) == 1024.0);
    CHECK(as_flt(g.run_scalar("return math.pow(2, 0.5)")) ==
          doctest::Approx(1.4142135623730951));
    CHECK(as_flt(g.run_scalar("return math.pow(3, 2)")) == 9.0);
}

TEST_CASE("evaluator: math.min/max subtype preservation")
{
    EvalRig g;
    // min — the chosen value's subtype is preserved
    CHECK(as_int(g.run_scalar("return math.min(5, 3, 8)")) == 3);
    // smallest is 3 (an int), so result is int even with float args
    CHECK(as_int(g.run_scalar("return math.min(5.0, 3, 8)")) == 3);
    // smallest is 0.5 (a float), result is float
    CHECK(as_flt(g.run_scalar("return math.min(0.5, 3, 8)")) == 0.5);
    CHECK(as_int(g.run_scalar("return math.min(-1, 0, 1)")) == -1);
    // max
    CHECK(as_int(g.run_scalar("return math.max(5, 3, 8)")) == 8);
    CHECK(as_flt(g.run_scalar("return math.max(5.0, 3.0, 8.0)")) == 8.0);
    CHECK(as_int(g.run_scalar("return math.max(5.0, 3, 8)")) == 8);
    // single-arg
    CHECK(as_int(g.run_scalar("return math.min(42)")) == 42);
    CHECK(as_int(g.run_scalar("return math.max(42)")) == 42);
    // negative numbers
    CHECK(as_int(g.run_scalar("return math.max(-5, -3)")) == -3);
}

TEST_CASE("evaluator: math.tointeger")
{
    EvalRig g;
    // int passes through
    CHECK(as_int(g.run_scalar("return math.tointeger(5)")) == 5);
    // integral float is converted
    CHECK(as_int(g.run_scalar("return math.tointeger(5.0)")) == 5);
    // non-integral float returns nil
    CHECK(g.run_scalar("return math.tointeger(5.5)").is_nil());
    // non-number returns nil
    CHECK(g.run_scalar("return math.tointeger('5')").is_nil());
    CHECK(g.run_scalar("return math.tointeger(nil)").is_nil());
}

TEST_CASE("evaluator: math.type")
{
    EvalRig g;
    CHECK(as_str(g.run_scalar("return math.type(5)")) == "integer");
    CHECK(as_str(g.run_scalar("return math.type(5.0)")) == "float");
    CHECK(as_str(g.run_scalar("return math.type(5.5)")) == "float");
    CHECK(g.run_scalar("return math.type('5')").is_nil());
    CHECK(g.run_scalar("return math.type(nil)").is_nil());
    CHECK(g.run_scalar("return math.type({})").is_nil());
}

TEST_CASE("evaluator: math.random — float form [0,1)")
{
    EvalRig g;
    // After seeding, results are reproducible
    g.run("math.randomseed(1)");
    LuaValue v1 = g.run_scalar("return math.random()");
    REQUIRE(v1.is_flt());
    double d = v1.as_flt();
    CHECK(d >= 0.0);
    CHECK(d < 1.0);
    // Same seed -> same value
    g.run("math.randomseed(1)");
    CHECK(as_flt(g.run_scalar("return math.random()")) == d);
}

TEST_CASE("evaluator: math.random — integer forms")
{
    EvalRig g;
    g.run("math.randomseed(42)");
    // random(n) -> integer in [1, n]
    for (int i = 0; i < 50; ++i) {
        LuaValue v = g.run_scalar("return math.random(6)");
        REQUIRE(v.is_int());
        long long r = v.as_int();
        CHECK(r >= 1);
        CHECK(r <= 6);
    }
    // random(m, n) -> integer in [m, n]
    for (int i = 0; i < 50; ++i) {
        LuaValue v = g.run_scalar("return math.random(10, 20)");
        REQUIRE(v.is_int());
        long long r = v.as_int();
        CHECK(r >= 10);
        CHECK(r <= 20);
    }
}

TEST_CASE("evaluator: math.random — empty interval errors")
{
    EvalRig g;
    auto r = g.run(
        "local ok, err = pcall(math.random, 5, 2); "
        "return ok, tostring(err)");
    REQUIRE(r.size() == 2);
    CHECK(as_bool(r[0]) == false);
    CHECK(r[1].as_str()->data.find("interval is empty") != std::string::npos);
}

TEST_CASE("evaluator: math.randomseed — reproducibility across reseed")
{
    EvalRig g;
    g.run("math.randomseed(123)");
    long long a = as_int(g.run_scalar("return math.random(1, 1000000)"));
    long long b = as_int(g.run_scalar("return math.random(1, 1000000)"));
    g.run("math.randomseed(123)");
    long long c = as_int(g.run_scalar("return math.random(1, 1000000)"));
    long long d = as_int(g.run_scalar("return math.random(1, 1000000)"));
    CHECK(a == c);
    CHECK(b == d);
    // And different seeds yield different sequences (probabilistic, but for
    // two unrelated seeds the first draw should differ).
    g.run("math.randomseed(456)");
    long long e = as_int(g.run_scalar("return math.random(1, 1000000)"));
    CHECK(e != a);
}

// =====================================================================
// M3.2.5: string.pack / unpack / packsize
// =====================================================================

TEST_CASE("evaluator: string.pack/unpack basic round-trip")
{
    EvalRig g;
    // i4 round-trip
    CHECK(as_int(g.run_scalar(
        "return string.unpack('i4', string.pack('i4', 2001))")) == 2001);
    // j (lua_Integer) round-trip
    CHECK(as_int(g.run_scalar(
        "return string.unpack('>j', string.pack('>j', 9007199254740992))")) ==
        9007199254740992LL);
    // b/B byte round-trip
    CHECK(as_int(g.run_scalar(
        "return string.unpack('b', string.pack('b', -12))")) == -12);
    CHECK(as_int(g.run_scalar(
        "return string.unpack('B', string.pack('B', 255))")) == 255);
    // double round-trip
    CHECK(as_flt(g.run_scalar(
        "return string.unpack('d', string.pack('d', 3.14159))")) ==
        doctest::Approx(3.14159));
}

TEST_CASE("evaluator: string.pack endianness")
{
    EvalRig g;
    // Little-endian i2 of 1 -> "\1\0"
    CHECK(as_str(g.run_scalar("return string.pack('<i2', 1)")) ==
        std::string("\1\0", 2));
    // Big-endian i2 of 1 -> "\0\1"
    CHECK(as_str(g.run_scalar("return string.pack('>i2', 1)")) ==
        std::string("\0\1", 2));
    // Native endianness detectable: pack('i2', 1) matches one of them
    auto native = as_str(g.run_scalar("return string.pack('i2', 1)"));
    bool is_little = (native == std::string("\1\0", 2));
    bool is_big = (native == std::string("\0\1", 2));
    CHECK(is_little != is_big);   // exactly one must be true
}

TEST_CASE("evaluator: string.pack/unpack mixed endian")
{
    EvalRig g;
    // ">i2 <i2" packs first big, second little
    // 10 big-endian i2 = \0\x0A, 20 little-endian i2 = \x14\0
    CHECK(as_str(g.run_scalar("return string.pack('>i2 <i2', 10, 20)")) ==
        std::string("\x00\x0A\x14\x00", 4));
    // Unpack the mixed format: <i2 >i2 of "\x0A\x00\x00\x14" = 10, 20
    auto r = g.run("return string.unpack('<i2 >i2', '\\10\\0\\0\\20')");
    REQUIRE(r.size() == 3);   // 2 values + pos
    CHECK(as_int(r[0]) == 10);
    CHECK(as_int(r[1]) == 20);
    CHECK(as_int(r[2]) == 5);   // 1-based pos after 4 bytes
}

TEST_CASE("evaluator: string.unpack sign extension")
{
    EvalRig g;
    // i1 of \255 = -1
    CHECK(as_int(g.run_scalar("return string.unpack('<i1', '\\255')")) == -1);
    // i2 of \255\255 = -1
    CHECK(as_int(g.run_scalar("return string.unpack('<i2', '\\255\\255')")) == -1);
    // i2 of \0\255 (big-endian) = -1
    CHECK(as_int(g.run_scalar("return string.unpack('>i2', '\\0\\255')")) == 255);
    // I2 (unsigned) of \255\255 = 65535
    CHECK(as_int(g.run_scalar("return string.unpack('<I2', '\\255\\255')")) == 65535);
    // i1 of \240 = -16
    CHECK(as_int(g.run_scalar("return string.unpack('<i1', '\\240')")) == -16);
}

TEST_CASE("evaluator: string.packsize basics")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("return string.packsize('i4')")) == 4);
    CHECK(as_int(g.run_scalar("return string.packsize('j')")) == 8);
    CHECK(as_int(g.run_scalar("return string.packsize('bBhHiIjJlL')")) ==
        1+1+2+2+4+4+8+8+8+8);
    CHECK(as_int(g.run_scalar("return string.packsize('f')")) == 4);
    CHECK(as_int(g.run_scalar("return string.packsize('d')")) == 8);
    CHECK(as_int(g.run_scalar("return string.packsize('c10')")) == 10);
    // x is 1 padding byte
    CHECK(as_int(g.run_scalar("return string.packsize('xi4')")) == 5);
    // Alignment
    CHECK(as_int(g.run_scalar("return string.packsize('!4 i4 i4')")) == 8);
    CHECK(as_int(g.run_scalar("return string.packsize('!4 i4 i2 i4')")) == 12);
}

TEST_CASE("evaluator: string.packsize matches #pack for fixed formats")
{
    EvalRig g;
    // Each pair: (format, full pack call). Verify packsize matches #pack.
    struct Case { const char* fmt; const char* pack_call; };
    Case cases[] = {
        {"i4",            "return #string.pack('i4', 0)"},
        {">j",            "return #string.pack('>j', 0)"},
        {"<bBhH",         "return #string.pack('<bBhH', 0, 0, 0, 0)"},
        {"ddd",           "return #string.pack('ddd', 0, 0, 0)"},
        {"c8",            "return #string.pack('c8', '')"},
        {"xi4xi4",        "return #string.pack('xi4xi4', 0, 0)"},
        {"!8 i4 i8 c1",   "return #string.pack('!8 i4 i8 c1', 0, 0, 'x')"},
    };
    for (auto& c : cases) {
        std::string sz = std::string("return string.packsize('") + c.fmt + "')";
        CHECK_MESSAGE(
            as_int(g.run_scalar(c.pack_call)) == as_int(g.run_scalar(sz)),
            "fmt: " << c.fmt);
    }
}

TEST_CASE("evaluator: string.pack float/double")
{
    EvalRig g;
    // float (4 bytes)
    CHECK(as_flt(g.run_scalar(
        "return string.unpack('<f', string.pack('<f', 1.5))")) ==
        doctest::Approx(1.5));
    // double (8 bytes)
    CHECK(as_flt(g.run_scalar(
        "return string.unpack('<d', string.pack('<d', 3.141592653589793))")) ==
        doctest::Approx(3.141592653589793));
    // n (native number == double for us)
    CHECK(as_flt(g.run_scalar(
        "return string.unpack('n', string.pack('n', 2.718281828459045))")) ==
        doctest::Approx(2.718281828459045));
    // Byte reversal: pack(">f", n) == pack("<f", n):reverse()
    CHECK(as_bool(g.run_scalar(
        "return string.pack('>f', 24.0) == string.pack('<f', 24.0):reverse()")) == true);
    CHECK(as_bool(g.run_scalar(
        "return string.pack('>d', 24.0) == string.pack('<d', 24.0):reverse()")) == true);
}

TEST_CASE("evaluator: string.pack/unpack strings")
{
    EvalRig g;
    // z (zero-terminated)
    CHECK(as_str(g.run_scalar(
        "return string.unpack('z', string.pack('z', 'hello') .. 'trailing')")) ==
        "hello");
    // s (length-prefixed)
    CHECK(as_str(g.run_scalar(
        "return string.unpack('s', string.pack('s', 'world'))")) == "world");
    // s1 (1-byte length prefix)
    CHECK(as_str(g.run_scalar(
        "return string.unpack('s1', string.pack('s1', 'ab'))")) == "ab");
    // cN (fixed-length)
    CHECK(as_str(g.run_scalar(
        "return string.unpack('c5', string.pack('c5', 'ab'))")) ==
        std::string("ab\0\0\0", 5));
    // c0 (empty)
    CHECK(as_str(g.run_scalar(
        "return string.unpack('c0', '')")) == "");
    // c8 pads with zeros
    CHECK(as_str(g.run_scalar(
        "return string.unpack('c8', string.pack('c8', 'abcd'))")) ==
        std::string("abcd\0\0\0\0", 8));
}

TEST_CASE("evaluator: string.pack/unpack alignment")
{
    EvalRig g;
    // No alignment by default: " < i1 i2 " -> 3 bytes
    CHECK(as_str(g.run_scalar("return string.pack(' < i1 i2 ', 2, 3)")) ==
        std::string("\2\3\0", 3));
    // !8 with b, Xh, i4, i8, c1 — the tricky composite alignment from tpack.lua
    auto x = as_str(g.run_scalar(
        "return string.pack('>!8 b Xh i4 i8 c1 Xi8', -12, 100, 200, '\\236')"));
    CHECK(x.size() == 24);
    // Unpack with the mirror format
    auto r = g.run(
        "return string.unpack('>!8 c1 Xh i4 i8 b Xi8 XI XH', '"
        "\\244\\0\\0\\0\\0\\0\\0\\100\\0\\0\\0\\0\\0\\0\\0\\200\\236"
        "\\0\\0\\0\\0\\0\\0\\0')");
    REQUIRE(r.size() == 5);   // 4 values + pos
    CHECK(as_str(r[0]) == std::string("\xf4", 1));   // Lua \244 = 0xF4
    CHECK(as_int(r[1]) == 100);
    CHECK(as_int(r[2]) == 200);
    CHECK(as_int(r[3]) == -20);
    CHECK(as_int(r[4]) == 25);   // pos (1-based) = 24 + 1
}

TEST_CASE("evaluator: string.pack/unpack with pos argument")
{
    EvalRig g;
    // Default pos = 1
    auto r = g.run("return string.unpack('i4i4', string.pack('i4i4', 10, 20))");
    REQUIRE(r.size() == 3);
    CHECK(as_int(r[0]) == 10);
    CHECK(as_int(r[1]) == 20);
    CHECK(as_int(r[2]) == 9);   // 1-based pos after 8 bytes
    // Explicit pos
    r = g.run("return string.unpack('i4', '\\1\\0\\0\\0\\2\\0\\0\\0', 5)");
    REQUIRE(r.size() == 2);
    CHECK(as_int(r[0]) == 2);
    CHECK(as_int(r[1]) == 9);
    // Negative pos
    r = g.run("return string.unpack('i4', '\\1\\0\\0\\0\\2\\0\\0\\0', -4)");
    REQUIRE(r.size() == 2);
    CHECK(as_int(r[0]) == 2);
    CHECK(as_int(r[1]) == 9);
}

TEST_CASE("evaluator: string.pack error cases")
{
    EvalRig g;
    auto run_pcall = [&](const std::string& code) -> std::string {
        auto r = g.run(code);
        REQUIRE(r.size() >= 2);
        REQUIRE(as_bool(r[0]) == false);
        REQUIRE(r[1].is_str());
        return r[1].as_str()->data;
    };
    // packsize on variable-length format
    CHECK(run_pcall(
        "local ok, err = pcall(string.packsize, 's'); return ok, tostring(err)")
        .find("variable-length format") != std::string::npos);
    // missing size for c
    CHECK(run_pcall(
        "local ok, err = pcall(string.packsize, 'c'); return ok, tostring(err)")
        .find("missing size") != std::string::npos);
    // integer overflow (signed byte)
    CHECK(run_pcall(
        "local ok, err = pcall(string.pack, 'b', 200); return ok, tostring(err)")
        .find("integer overflow") != std::string::npos);
    // unsigned overflow
    CHECK(run_pcall(
        "local ok, err = pcall(string.pack, 'B', -1); return ok, tostring(err)")
        .find("unsigned overflow") != std::string::npos);
    // invalid format option
    CHECK(run_pcall(
        "local ok, err = pcall(string.packsize, 'r'); return ok, tostring(err)")
        .find("invalid format option") != std::string::npos);
    // string longer than c size
    CHECK(run_pcall(
        "local ok, err = pcall(string.pack, 'c3', '1234'); return ok, tostring(err)")
        .find("longer than") != std::string::npos);
    // data string too short
    CHECK(run_pcall(
        "local ok, err = pcall(string.unpack, 'i4', 'ab'); return ok, tostring(err)")
        .find("too short") != std::string::npos);
    // invalid next option for X
    CHECK(run_pcall(
        "local ok, err = pcall(string.packsize, 'X'); return ok, tostring(err)")
        .find("invalid next option") != std::string::npos);
    // not power of 2 (i3 has size 3)
    CHECK(run_pcall(
        "local ok, err = pcall(string.packsize, '!4i3'); return ok, tostring(err)")
        .find("not power of 2") != std::string::npos);
    // 16-byte integer overflow on unpack: non-canonical bytes (0x03 in the
    // high bytes of a positive value; 0x03 != 0x00 sign-extension).
    CHECK(run_pcall(
        "local ok, err = pcall(string.unpack, 'i16', string.rep('\\3', 16)); "
        "return ok, tostring(err)")
        .find("does not fit") != std::string::npos);
}

TEST_CASE("evaluator: string.pack oversize integer round-trip")
{
    EvalRig g;
    // size > SZINT (8): up to 16 bytes with canonical sign extension.
    // Positive value, high bytes are 0x00.
    CHECK(as_int(g.run_scalar(
        "return string.unpack('<i9', string.pack('<i9', 42))")) == 42);
    // Negative value, high bytes are 0xFF.
    CHECK(as_int(g.run_scalar(
        "return string.unpack('<i10', string.pack('<i10', -1))")) == -1);
    // maxinteger as i16
    CHECK(as_int(g.run_scalar(
        "return string.unpack('<j', string.pack('<j', math.maxinteger))")) ==
        std::numeric_limits<long long>::max());
}

// =====================================================================
// M3.5 Part A: _ENV / _G semantics
// =====================================================================

TEST_CASE("evaluator: _G basic identity")
{
    EvalRig g;
    CHECK(g.run_scalar("return _G").is_table());
    CHECK(as_bool(g.run_scalar("return _G._G == _G")) == true);
    CHECK(as_str(g.run_scalar("return _VERSION")) == "Lua 5.4");
}

TEST_CASE("evaluator: global read/write via _G")
{
    EvalRig g;
    CHECK(as_int(g.run_scalar("x = 10; return _G.x")) == 10);
    CHECK(as_int(g.run_scalar("x = 10; return rawget(_G, 'x')")) == 10);
    CHECK(as_int(g.run_scalar("_G.y = 20; return y")) == 20);
    // assignment creates globals
    CHECK(as_int(g.run_scalar("x = 42; return x")) == 42);
}

TEST_CASE("evaluator: local _ENV rebinds scope")
{
    EvalRig g;
    // local _ENV = {} creates a sandbox: writes go to the new table.
    CHECK(as_int(g.run_scalar(
        "local _ENV = {}; x = 1; return x")) == 1);
    // Original _G is not affected.
    CHECK(g.run_scalar("local _ENV = {}; return _G").is_nil());
    // Outer _G survives after the block exits.
    CHECK(as_bool(g.run_scalar(
        "do local _ENV = {} end; return _G == _G")) == true);
    // Global access inside the sandbox reads from the new _ENV.
    CHECK(g.run_scalar(
        "do local _ENV = {}; a = 1 end; return a").is_nil());
}

TEST_CASE("evaluator: _ENV metamethods")
{
    EvalRig g;
    // __index on _ENV for global fallback.
    CHECK(as_int(g.run_scalar(
        "setmetatable(_ENV, {__index = function() return 42 end}); "
        "return nonexistent")) == 42);
    // __newindex on _ENV for global write interception.
    CHECK(as_int(g.run_scalar(
        "setmetatable(_ENV, {__newindex = function(t,k,v) "
        "rawset(t, 'wrapped_'..k, v) end}); "
        "x = 1; return wrapped_x")) == 1);
}

TEST_CASE("evaluator: closure captures _ENV")
{
    EvalRig g;
    // A closure captures _ENV at creation time.
    CHECK(as_bool(g.run_scalar(
        "function f() return _ENV end; return f() == _G")) == true);
    // local _ENV in a function body.
    CHECK(as_int(g.run_scalar(
        "function f(x) local _ENV = {g = x}; return g end; "
        "return f(99)")) == 99);
    // A closure created inside a sandboxed scope captures the sandbox _ENV.
    CHECK(as_int(g.run_scalar(
        "local r; do local _ENV = {g = 99}; "
        "r = function() return g end end; "
        "return r()")) == 99);
}

TEST_CASE("evaluator: _ENV as table field (not the global)")
{
    EvalRig g;
    // `_ENV` as a table field is NOT the global _ENV (Lua §3.3.2).
    CHECK(as_int(g.run_scalar(
        "local a = {_ENV = {x = 5}}; return a._ENV.x")) == 5);
}

TEST_CASE("evaluator: _ENV = nil causes global access errors")
{
    EvalRig g;
    // _ENV = nil makes subsequent global reads error ("attempt to index a nil value").
    auto r = g.run("local ok, err = pcall(function() _ENV = nil; return x end); "
                   "return ok, tostring(err)");
    CHECK(as_bool(r[0]) == false);
}

TEST_CASE("evaluator: _ENV rebind via assignment")
{
    EvalRig g;
    // _ENV = tbl rebinds globals for the current scope.
    CHECK(as_int(g.run_scalar(
        "local t = {}; _ENV = t; x = 1; return t.x")) == 1);
}

// =====================================================================
// M3.5 Part B: load / loadstring / dofile
// =====================================================================

TEST_CASE("evaluator: load basic")
{
    EvalRig g;
    // load + call.
    CHECK(as_int(g.run_scalar("return (load('return 42'))()")) == 42);
    // loadstring alias.
    CHECK(as_int(g.run_scalar("return (loadstring('return 99'))()")) == 99);
    // load returns a function.
    CHECK(g.run_scalar("return type(load('return 1'))").as_str()->data == "function");
}

TEST_CASE("evaluator: load syntax error")
{
    EvalRig g;
    // Syntax error returns nil + error message (does NOT throw).
    auto r = g.run("local f, err = load('if then'); return f, tostring(err)");
    CHECK(r[0].is_nil());
    CHECK(as_str(r[1]).find("token") != std::string::npos);
    // Non-string first arg.
    r = g.run("local f, err = load(42); return f, err");
    CHECK(r[0].is_nil());
}

TEST_CASE("evaluator: load vararg chunk")
{
    EvalRig g;
    // Loaded chunks are vararg: ... collects arguments.
    CHECK(as_int(g.run_scalar(
        "return (load('return ...'))(1, 2, 3)")) == 1);
    // select('#', ...) inside a loaded chunk.
    CHECK(as_int(g.run_scalar(
        "return select('#', (load('return ...'))())")) == 0);
}

TEST_CASE("evaluator: load with custom env")
{
    EvalRig g;
    // env argument controls _ENV for the loaded chunk.
    CHECK(as_int(g.run_scalar(
        "return (load('return x', 'chunk', 'bt', {x = 42}))()")) == 42);
    // Writes go to the env table.
    CHECK(as_int(g.run_scalar(
        "local env = {}; (load('x = 1', 'chunk', 'bt', env))(); return env.x")) == 1);
    // Default env is _G.
    CHECK(as_int(g.run_scalar(
        "x = 7; return (load('return x'))()")) == 7);
}

TEST_CASE("evaluator: load binary mode rejection")
{
    EvalRig g;
    auto r = g.run("local f, err = load('return 1', 'chunk', 'b'); "
                   "return f, tostring(err)");
    CHECK(r[0].is_nil());
    CHECK(as_str(r[1]).find("binary") != std::string::npos);
}

TEST_CASE("evaluator: load with closures inside")
{
    EvalRig g;
    // A loaded chunk can define and use closures.
    CHECK(as_int(g.run_scalar(
        "local f = load('local x = 0; return function() x = x + 1; return x end'); "
        "local c = f(); c(); c(); return c()")) == 3);
}

TEST_CASE("evaluator: dofile")
{
    EvalRig g;
    // Write a temp file and dofile it.
    {
        std::ofstream os("/tmp/yueshi_dofile_test.lua");
        os << "return 42\n";
    }
    CHECK(as_int(g.run_scalar(
        "return dofile('/tmp/yueshi_dofile_test.lua')")) == 42);

    // dofile with nonexistent file errors.
    auto r = g.run("local ok, err = pcall(dofile, '/tmp/yueshi_nonexistent.lua'); "
                   "return ok, tostring(err)");
    CHECK(as_bool(r[0]) == false);

    std::remove("/tmp/yueshi_dofile_test.lua");
}






