// Evaluator unit tests: lex -> parse -> Evaluator::run, assert on the
// returned LuaValues. Mirrors the parser-test pipeline (Tokenizer -> Parser ->
// parse()) and adds the run step. Evaluator output (print) is captured by
// injecting a std::ostringstream.
//
// House style: TEST_CASE("evaluator: ..."), SUBCASE, REQUIRE_MESSAGE. Helpers
// in an anonymous namespace; `using namespace ys::lua;` at the top.

#include "doctest.h"

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






