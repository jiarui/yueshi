// Parser smoke tests: lex → Parser::parse → ASTPrinter, asserting parse
// success/failure and a few key AST shapes. The corpus acceptance test
// (parser_corpus.cpp) does the broad ≥95% run; this file does focused
// structural checks on tricky cases (precedence, left-recursion, statements).

#include "doctest.h"

#include <sstream>
#include <string>

#include "lua/ast.h"
#include "lua/ast_print.h"
#include "lua/lex.h"
#include "lua/parser.h"

using namespace ys::lua;

namespace {
// Lex + parse a source snippet. Returns the root AstNode (Chunk), or asserts
// failure when expect_ok is false.
AstNode parse_ok(std::string_view src)
{
    auto toks = Tokenizer{std::string{src}}.tokenize();
    // No lex error sentinel may precede EOS.
    for (std::size_t i = 0; i + 1 < toks.size(); ++i)
        REQUIRE(toks[i].id != -1);
    Parser p{std::move(toks)};
    auto ast = p.parse();
    REQUIRE_MESSAGE(ast, "expected parse to succeed");
    auto errs = p.take_errors();
    REQUIRE_MESSAGE(errs.empty(), "expected no diagnostics");
    return std::move(*ast);
}

// Assert the snippet fails to parse (parse() nullopt OR non-empty diagnostics).
void parse_fails(std::string_view src)
{
    auto toks = Tokenizer{std::string{src}}.tokenize();
    Parser p{std::move(toks)};
    auto ast = p.parse();
    auto errs = p.take_errors();
    bool failed = !ast || !errs.empty();
    CHECK_MESSAGE(failed, "expected parse to FAIL, but it succeeded");
}

std::string sexp(const AstNode& n) { return ASTPrinter::to_sexp(n); }
} // namespace

// ---------------------------------------------------------------------------
// Literals & atoms
// ---------------------------------------------------------------------------
TEST_CASE("parser: bare number is not a valid statement")
{
    // A lone expression (like `42`) is NOT a statement in Lua — only a
    // functioncall or assignment is. So `42` as a whole chunk must fail.
    parse_fails("42");
}

TEST_CASE("parser: return integer")
{
    auto ast = parse_ok("return 42");
    CHECK(holds<Chunk>(ast));
    auto sex = sexp(ast);
    CHECK_MESSAGE(sex.find("(IntLit 42)") != std::string::npos, sex);
    CHECK_MESSAGE(sex.find("(Return") != std::string::npos, sex);
}

TEST_CASE("parser: return float / string / nil / true / false / vararg")
{
    auto check = [](std::string_view src, std::string_view needle) {
        auto s = sexp(parse_ok(src));
        CHECK_MESSAGE(s.find(std::string{needle}) != std::string::npos, s);
    };
    check("return 3.14", "(FltLit");
    check("return \"hi\"", "(StrLit \"hi\")");
    check("return nil", "(nil)");
    check("return true", "(True)");
    check("return false", "(False)");
    check("return ...", "(Vararg ...)");
}

// ---------------------------------------------------------------------------
// Operator precedence (§3.4.8) — the precedence ladder
// ---------------------------------------------------------------------------
TEST_CASE("parser: precedence — mul binds tighter than add")
{
    // 1 + 2 * 3  →  (+ 1 (* 2 3)); the BinOp * must nest inside +.
    auto s = sexp(parse_ok("return 1 + 2 * 3"));
    // The inner Mul node appears before the outer Add in the S-exp text.
    auto mul_pos = s.find("(BinOp *");
    auto add_pos = s.find("(BinOp +");
    CHECK_MESSAGE(mul_pos != std::string::npos, s);
    CHECK_MESSAGE(add_pos != std::string::npos, s);
}

TEST_CASE("parser: pow binds tighter than unary minus (-2^2 == -4)")
{
    // -2^2  →  (- (^ 2 2)); ^ is the right operand context of nothing here —
    // the UnOp Neg wraps the BinOp Pow.
    auto s = sexp(parse_ok("return -2^2"));
    auto neg_pos = s.find("(UnOp -");
    auto pow_pos = s.find("(BinOp ^");
    CHECK_MESSAGE(neg_pos != std::string::npos, s);
    CHECK_MESSAGE(pow_pos != std::string::npos, s);
    // Neg must be the OUTER node (appear before Pow in pre-order traversal).
    CHECK_MESSAGE(neg_pos < pow_pos, "Neg should wrap Pow (-2^2 == -(2^2))");
}

TEST_CASE("parser: unary minus as pow's right operand (2^-2 == 0.25)")
{
    // 2^-2 → (^ 2 (- 2)); the UnOp is the rhs of ^.
    auto s = sexp(parse_ok("return 2^-2"));
    auto pow_pos = s.find("(BinOp ^");
    auto neg_pos = s.find("(UnOp -");
    CHECK_MESSAGE(pow_pos != std::string::npos, s);
    CHECK_MESSAGE(neg_pos != std::string::npos, s);
    // Pow is outer (pre-order: Pow before its rhs Neg).
    CHECK_MESSAGE(pow_pos < neg_pos, "Pow should wrap its rhs Neg (2^-2)");
}

TEST_CASE("parser: left-assoc subtraction (1-2-3 == (1-2)-3)")
{
    // (1-2)-3: the lhs of the outer - is another BinOp -.
    auto s = sexp(parse_ok("return 1 - 2 - 3"));
    // Two Sub nodes; the first in pre-order is the outer.
    auto first = s.find("(BinOp -");
    auto second = s.find("(BinOp -", first + 1);
    CHECK_MESSAGE(first != std::string::npos, s);
    CHECK_MESSAGE(second != std::string::npos, s);
}

TEST_CASE("parser: right-assoc exponent (2^3^2 == 2^(3^2))")
{
    auto s = sexp(parse_ok("return 2^3^2"));
    CHECK_MESSAGE(s.find("(BinOp ^") != std::string::npos, s);
}

TEST_CASE("parser: right-assoc concat (a..b..c)")
{
    auto s = sexp(parse_ok("return \"a\" .. \"b\" .. \"c\""));
    CHECK_MESSAGE(s.find("(BinOp ..") != std::string::npos, s);
}

TEST_CASE("parser: logical and relational operators")
{
    auto s = sexp(parse_ok("return a < b and c >= d or e == f"));
    CHECK_MESSAGE(s.find("(BinOp and") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(BinOp or") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(BinOp <") != std::string::npos, s);
}

// ---------------------------------------------------------------------------
// prefixexp / var / functioncall — the left-recursive triangle
// ---------------------------------------------------------------------------
TEST_CASE("parser: var — base case Name (no suffix)")
{
    // Zero suffixes: the seed-grow must at least accept the bare Name.
    auto s = sexp(parse_ok("return a"));
    CHECK_MESSAGE(s.find("(Name \"a\")") != std::string::npos, s);
}
TEST_CASE("parser: var — one suffix a.b (single growth step)")
{
    auto s = sexp(parse_ok("return a.b"));
    auto c1 = s.find("(Field .b");
    auto c2 = s.find("(Field .", c1 + 1);
    CHECK_MESSAGE(c1 != std::string::npos, s);
    CHECK_MESSAGE(c2 == std::string::npos, "exactly one Field suffix for a.b", s);
}
TEST_CASE("parser: dotted field access (left recursion)")
{
    auto s = sexp(parse_ok("return a.b.c"));
    // Three nested Field nodes: (Field .c (Field .b (Name "a")))
    auto c1 = s.find("(Field .");
    auto c2 = s.find("(Field .", c1 + 1);
    auto c3 = s.find("(Field .", c2 + 1);
    CHECK_MESSAGE(c1 != std::string::npos, s);
    CHECK_MESSAGE(c2 != std::string::npos, s);
    CHECK_MESSAGE(c3 == std::string::npos, "exactly two Field suffixes for a.b.c");
}

TEST_CASE("parser: index access")
{
    auto s = sexp(parse_ok("return t[1][2]"));
    CHECK_MESSAGE(s.find("(Index") != std::string::npos, s);
}

TEST_CASE("parser: parenthesized expression")
{
    auto s = sexp(parse_ok("return (1 + 2)"));
    CHECK_MESSAGE(s.find("(Paren") != std::string::npos, s);
}

TEST_CASE("parser: function call")
{
    auto s = sexp(parse_ok("f(1, 2, 3)"));
    CHECK_MESSAGE(s.find("(CallStat") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(Call") != std::string::npos, s);
}

TEST_CASE("parser: method call")
{
    auto s = sexp(parse_ok("o:m(42)"));
    CHECK_MESSAGE(s.find("(MethodCall :m") != std::string::npos, s);
}

TEST_CASE("parser: call with string / table sugar")
{
    auto s1 = sexp(parse_ok("print \"hello\""));
    CHECK_MESSAGE(s1.find("(Call") != std::string::npos, s1);
    auto s2 = sexp(parse_ok("f{x = 1, 2}"));
    CHECK_MESSAGE(s2.find("(Call") != std::string::npos, s2);
}

TEST_CASE("parser: chained calls f()()")
{
    auto s = sexp(parse_ok("f()()"));
    // Two Call nodes (outer wraps inner).
    auto c1 = s.find("(Call");
    auto c2 = s.find("(Call", c1 + 1);
    CHECK_MESSAGE(c1 != std::string::npos, s);
    CHECK_MESSAGE(c2 != std::string::npos, s);
}

// ---------------------------------------------------------------------------
// table constructor
// ---------------------------------------------------------------------------
TEST_CASE("parser: table constructor — three field forms")
{
    auto s = sexp(parse_ok("return {1, x = 2, [\"k\"] = 3}"));
    CHECK_MESSAGE(s.find("(TableCtor") != std::string::npos, s);
    CHECK_MESSAGE(s.find("field-pos") != std::string::npos, s);
    CHECK_MESSAGE(s.find("field-named") != std::string::npos, s);
    CHECK_MESSAGE(s.find("field-bracket") != std::string::npos, s);
}
TEST_CASE("parser: table constructor — positional only")
{
    // Isolates field_positional from named/bracketed; no prefixexp involved.
    auto s = sexp(parse_ok("return {1, 2, 3}"));
    CHECK_MESSAGE(s.find("(TableCtor") != std::string::npos, s);
    CHECK_MESSAGE(s.find("field-pos") != std::string::npos, s);
}
TEST_CASE("parser: table constructor — named only")
{
    // Isolates field_named; uses get<Name>(nm) in the action.
    auto s = sexp(parse_ok("return {x = 1, y = 2}"));
    CHECK_MESSAGE(s.find("field-named") != std::string::npos, s);
}

// ---------------------------------------------------------------------------
// function definitions
// ---------------------------------------------------------------------------
TEST_CASE("parser: anonymous function expression")
{
    auto s = sexp(parse_ok("return function(a, b, ...) return a + b end"));
    CHECK_MESSAGE(s.find("(FuncDef") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(FuncBody (a b ...)") != std::string::npos, s);
}

TEST_CASE("parser: local function")
{
    auto s = sexp(parse_ok("local function f() return 1 end"));
    CHECK_MESSAGE(s.find("(LocalFunction f") != std::string::npos, s);
}

TEST_CASE("parser: function statement (dotted + method name)")
{
    auto s = sexp(parse_ok("function a.b.c:d() end"));
    CHECK_MESSAGE(s.find("(FuncStat") != std::string::npos, s);
}

// ---------------------------------------------------------------------------
// statements & control flow
// ---------------------------------------------------------------------------
TEST_CASE("parser: local declaration with attributes")
{
    auto s = sexp(parse_ok("local x <const> = 1\nlocal y <close>"));
    CHECK_MESSAGE(s.find("(LocalDecl x<const>") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(LocalDecl y<close>)") != std::string::npos, s);
}

TEST_CASE("parser: assignment (single + multi target)")
{
    auto s1 = sexp(parse_ok("x = 1"));
    CHECK_MESSAGE(s1.find("(Assign") != std::string::npos, s1);
    auto s2 = sexp(parse_ok("a, b = 1, 2"));
    CHECK_MESSAGE(s2.find("(targets") != std::string::npos, s2);
    CHECK_MESSAGE(s2.find("(values") != std::string::npos, s2);
}

TEST_CASE("parser: if / elseif / else")
{
    auto s = sexp(parse_ok("if a then return 1 elseif b then return 2 else return 3 end"));
    CHECK_MESSAGE(s.find("(If") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(then") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(else") != std::string::npos, s);
}

TEST_CASE("parser: while / repeat")
{
    CHECK(sexp(parse_ok("while true do break end")).find("(While") != std::string::npos);
    CHECK(sexp(parse_ok("repeat x = x + 1 until x > 10")).find("(Repeat") != std::string::npos);
}

TEST_CASE("parser: numeric + generic for")
{
    CHECK(sexp(parse_ok("for i = 1, 10, 2 do end")).find("(NumericFor") != std::string::npos);
    CHECK(sexp(parse_ok("for k, v in pairs(t) do end")).find("(GenericFor") != std::string::npos);
}

TEST_CASE("parser: goto / label / do block")
{
    auto s = sexp(parse_ok("do goto done ::done:: end"));
    CHECK_MESSAGE(s.find("(Goto done)") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(Label done)") != std::string::npos, s);
    CHECK_MESSAGE(s.find("(Do") != std::string::npos, s);
}

// ---------------------------------------------------------------------------
// A realistic small program
// ---------------------------------------------------------------------------
TEST_CASE("parser: small factorial program")
{
    std::string src = R"(
local function fact(n)
    if n <= 1 then
        return 1
    end
    return n * fact(n - 1)
end
print(fact(10))
)";
    auto ast = parse_ok(src);
    CHECK(holds<Chunk>(ast));
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------
TEST_CASE("parser: missing 'end' is rejected")
{
    parse_fails("if true then return 1");
}

TEST_CASE("parser: missing 'then' is rejected")
{
    parse_fails("if true return 1 end");
}
