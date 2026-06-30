// Structural parser tests: lex → Parser::parse → direct AstNode walk.
//
// Unlike parser_test.cpp (which asserts on S-expression text substrings — many
// of those checks pass on a structurally *wrong* tree), these tests walk the
// typed variant via the helpers in ast_check.h and pin down the exact tree
// shape: precedence nesting direction, left/right associativity, suffix-loop
// folding (the parser's biggest deviation from the reference grammar),
// statement child-attachment, params, tables, and source ranges.
//
// A parser that produces a syntactically-valid but wrong tree cannot pass
// these — the assertions name the exact alternative and the exact child slot.

#include "doctest.h"

#include <string>
#include <string_view>

#include "ast_check.h"
#include "lua/ast.h"
#include "lua/lex.h"
#include "lua/parser.h"

using namespace ys::lua;
using namespace ys::lua::test_ast;

namespace {

// Lex + parse a snippet; REQUIRE a clean parse and return the Chunk root.
AstNode parse_ok(std::string_view src)
{
    auto toks = Tokenizer{std::string{src}}.tokenize();
    for (std::size_t i = 0; i + 1 < toks.size(); ++i)
        REQUIRE(toks[i].id != -1);
    Parser p{std::move(toks)};
    auto ast = p.parse();
    REQUIRE_MESSAGE(ast, "expected parse to succeed");
    auto errs = p.take_errors();
    REQUIRE_MESSAGE(errs.empty(), "expected no diagnostics");
    return std::move(*ast);
}

// NodeRef owns the parse-tree root and exposes a non-owning view of a single
// node within it. The helpers below (ret / one_stat / block_ok) return this by
// value so the root lives as long as the caller's local; the previous design
// returned a bare pointer INTO a root owned by the helper's local, a dangling
// pointer that caused use-after-free (garbage .size(), SIGSEGV).
struct NodeRef {
    AstNode root;                // keeps the whole tree alive
    const AstNode* node;         // the node of interest within `root`
    const AstNode& operator*() const { return *node; }
};

// Parse "return <expr>" and return the single returned expression node.
NodeRef ret(std::string_view expr_src)
{
    NodeRef out{parse_ok(std::string{"return "} + std::string{expr_src}), nullptr};
    auto* s = stmt_at(out.root, 0);
    REQUIRE_MESSAGE(s, "expected one top-level statement");
    auto* r = return_stat(*s);
    REQUIRE_MESSAGE(r, "expected a Return statement");
    auto* vals = return_values(*s);
    if (!vals || vals->size() != 1) { REQUIRE(false); }
    out.node = raw((*vals)[0]);
    return out;
}

// Parse a single top-level statement (no trailing return).
NodeRef one_stat(std::string_view src)
{
    NodeRef out{parse_ok(src), nullptr};
    auto* s = stmt_at(out.root, 0);
    REQUIRE_MESSAGE(s, "expected one top-level statement");
    out.node = s;
    return out;
}

} // namespace

// ===========================================================================
// A. Precedence — every adjacent boundary (nesting direction)
//    For each adjacent operator-priority pair, the higher-binding operator
//    must nest INSIDE the lower-binding one. We prove this by asserting which
//    child slot of the outer node recurses into another BinOp.
// ===========================================================================
TEST_CASE("parser: precedence nesting at every boundary")
{
    SUBCASE("or < and: 1 or 2 and 3 -> Or(1, And(2,3))")
    {
        auto e = ret("1 or 2 and 3");
        REQUIRE(is_binop(*e, BinOpKind::Or));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::And));  // rhs recurses
        REQUIRE(is_name(*binop_lhs(*e), "1") == false);     // sanity: lhs is int
    }
    SUBCASE("and < relational: 1 and 2 < 3 -> And(1, Lt(2,3))")
    {
        auto e = ret("1 and 2 < 3");
        REQUIRE(is_binop(*e, BinOpKind::And));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Lt));
    }
    SUBCASE("relational < bitor: 1 < 2 | 3 -> Lt(1, BOr(2,3))")
    {
        auto e = ret("1 < 2 | 3");
        REQUIRE(is_binop(*e, BinOpKind::Lt));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::BOr));
    }
    SUBCASE("bitor < bxor: 1 | 2 ~ 3 -> BOr(1, BXor(2,3))")
    {
        auto e = ret("1 | 2 ~ 3");
        REQUIRE(is_binop(*e, BinOpKind::BOr));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::BXor));
    }
    SUBCASE("bxor < band: 1 ~ 2 & 3 -> BXor(1, BAnd(2,3))")
    {
        auto e = ret("1 ~ 2 & 3");
        REQUIRE(is_binop(*e, BinOpKind::BXor));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::BAnd));
    }
    SUBCASE("band < shift: 1 & 2 << 3 -> BAnd(1, Shl(2,3))")
    {
        auto e = ret("1 & 2 << 3");
        REQUIRE(is_binop(*e, BinOpKind::BAnd));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Shl));
    }
    SUBCASE("shift < concat: 'x' .. 2 .. 3 has concat above... use 1 .. 2 + 3")
    {
        // concat is lower than shift but higher than add; test concat<add here.
        auto e = ret("1 .. 2 + 3");
        REQUIRE(is_binop(*e, BinOpKind::Concat));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Add));
    }
    SUBCASE("shift < concat: 1 << 2 .. 3 -> Shl(1, Concat(2,3))")
    {
        auto e = ret("1 << \"2\" .. \"3\"");
        REQUIRE(is_binop(*e, BinOpKind::Shl));
        // rhs is concat of two strings (numbers aren't concat-able, but the
        // grammar parses by precedence regardless of type checking).
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Concat));
    }
    SUBCASE("concat < add: 1 .. 2 + 3 -> Concat(1, Add(2,3))")
    {
        auto e = ret("1 .. 2 + 3");
        REQUIRE(is_binop(*e, BinOpKind::Concat));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Add));
    }
    SUBCASE("add < mul: 1 + 2 * 3 -> Add(1, Mul(2,3))")
    {
        auto e = ret("1 + 2 * 3");
        REQUIRE(is_binop(*e, BinOpKind::Add));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Mul));
        // and the lhs is NOT a binop
        REQUIRE(binop(*binop_lhs(*e)) == nullptr);
    }
    SUBCASE("mul < unary: 2 * -3 -> Mul(2, Neg(3))")
    {
        auto e = ret("2 * -3");
        REQUIRE(is_binop(*e, BinOpKind::Mul));
        REQUIRE(is_unop(*binop_rhs(*e), UnOpKind::Neg));
    }
    SUBCASE("unary < pow: -2^2 -> Neg(Pow(2,2))")
    {
        auto e = ret("-2^2");
        REQUIRE(is_unop(*e, UnOpKind::Neg));
        REQUIRE(is_binop(*unop_operand(*e), BinOpKind::Pow));
    }
    SUBCASE("mul < pow: 2 * 3 ^ 2 -> Mul(2, Pow(3,2))")
    {
        auto e = ret("2 * 3 ^ 2");
        REQUIRE(is_binop(*e, BinOpKind::Mul));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Pow));
    }
    SUBCASE("unary binds tighter than mul (lhs): -a * b -> Mul(Neg(a), b)")
    {
        auto e = ret("-a * b");
        REQUIRE(is_binop(*e, BinOpKind::Mul));
        REQUIRE(is_unop(*binop_lhs(*e), UnOpKind::Neg));
        REQUIRE(is_name(*unop_operand(*binop_lhs(*e)), "a"));
    }
    SUBCASE("floor-div and mod share add level: 1 + 2 // 3")
    {
        auto e = ret("1 + 2 // 3");
        REQUIRE(is_binop(*e, BinOpKind::Add));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::FloorDiv));
    }
}

// ===========================================================================
// B. Associativity — prove the DIRECTION by checking which child recurses.
//    Left-assoc:  a OP a OP a  =>  outer.lhs recurses, outer.rhs is the last.
//    Right-assoc: a OP a OP a  =>  outer.rhs recurses, outer.lhs is the first.
// ===========================================================================
TEST_CASE("parser: left-associativity")
{
    SUBCASE("subtraction: 1-2-3 -> Sub(Sub(1,2),3)")
    {
        auto e = ret("1 - 2 - 3");
        REQUIRE(is_binop(*e, BinOpKind::Sub));
        // lhs recurses, rhs is the last literal (3)
        REQUIRE(is_binop(*binop_lhs(*e), BinOpKind::Sub));
        REQUIRE(intlit(*binop_rhs(*e)) != nullptr);
        // and inner Sub's operands are 1 and 2 (not 2 and 3)
        auto* inner = binop_lhs(*e);  // AstNode* — reuse for further navigation
        REQUIRE(intlit(*binop_lhs(*inner)) != nullptr);
        REQUIRE(intlit(*binop_rhs(*inner)) != nullptr);
    }
    SUBCASE("floor-div: 64//2//2 -> FloorDiv(FloorDiv(64,2),2)")
    {
        auto e = ret("64 // 2 // 2");
        REQUIRE(is_binop(*e, BinOpKind::FloorDiv));
        REQUIRE(is_binop(*binop_lhs(*e), BinOpKind::FloorDiv));
        REQUIRE(intlit(*binop_rhs(*e)) != nullptr);
    }
    SUBCASE("modulo: 100%7%3 -> Mod(Mod(100,7),3)")
    {
        auto e = ret("100 % 7 % 3");
        REQUIRE(is_binop(*e, BinOpKind::Mod));
        REQUIRE(is_binop(*binop_lhs(*e), BinOpKind::Mod));
    }
    SUBCASE("left-shift: 1<<2<<3 -> Shl(Shl(1,2),3)")
    {
        auto e = ret("1 << 2 << 3");
        REQUIRE(is_binop(*e, BinOpKind::Shl));
        REQUIRE(is_binop(*binop_lhs(*e), BinOpKind::Shl));
    }
    SUBCASE("and: a and b and c -> And(And(a,b),c)")
    {
        auto e = ret("a and b and c");
        REQUIRE(is_binop(*e, BinOpKind::And));
        REQUIRE(is_binop(*binop_lhs(*e), BinOpKind::And));
        REQUIRE(is_name(*binop_rhs(*e), "c"));
    }
    SUBCASE("or: a or b or c -> Or(Or(a,b),c)")
    {
        auto e = ret("a or b or c");
        REQUIRE(is_binop(*e, BinOpKind::Or));
        REQUIRE(is_binop(*binop_lhs(*e), BinOpKind::Or));
        REQUIRE(is_name(*binop_rhs(*e), "c"));
    }
    SUBCASE("relational chains left: 1 < 2 and 2 < 3 nesting")
    {
        // relational itself is non-associative in Lua (a<b<c is a syntax
        // error), so test it chains via and/or as left-assoc instead.
        auto e = ret("a < b and b < c");
        REQUIRE(is_binop(*e, BinOpKind::And));
        REQUIRE(is_binop(*binop_lhs(*e), BinOpKind::Lt));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Lt));
    }
}

TEST_CASE("parser: right-associativity")
{
    SUBCASE("exponent: 2^3^2 -> Pow(2, Pow(3,2))")
    {
        auto e = ret("2^3^2");
        REQUIRE(is_binop(*e, BinOpKind::Pow));
        // rhs recurses (right-assoc), lhs is the first operand (2)
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Pow));
        REQUIRE(intlit(*binop_lhs(*e)) != nullptr);
        // inner Pow's operands are 3 and 2
        auto* inner = binop_rhs(*e);  // AstNode* — reuse for further navigation
        REQUIRE(intlit(*binop_lhs(*inner)) != nullptr);
        REQUIRE(intlit(*binop_rhs(*inner)) != nullptr);
    }
    SUBCASE("concat: 'a'..'b'..'c' -> Concat('a', Concat('b','c'))")
    {
        // The classic Lua gotcha: concat is RIGHT-assoc, so a..b..c groups as
        // a..(b..c). The old substring test never verified this direction.
        auto e = ret("\"a\" .. \"b\" .. \"c\"");
        REQUIRE(is_binop(*e, BinOpKind::Concat));
        REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Concat));
        // lhs is the first string "a"
        REQUIRE(strlit(*binop_lhs(*e)) != nullptr);
        // inner rhs concat's operands are "b" and "c"
        auto* inner = binop_rhs(*e);  // AstNode* — reuse for further navigation
        REQUIRE(strlit(*binop_lhs(*inner)) != nullptr);
        REQUIRE(strlit(*binop_rhs(*inner)) != nullptr);
    }
}

// ===========================================================================
// C. Suffix folding — the prefixexp rewrite (parser's biggest deviation from
//    the reference grammar, which is left-recursive). Each suffix must
//    left-fold so a.b.c groups as ((a.b).c) and f()() as ((f())()).
// ===========================================================================
TEST_CASE("parser: suffix folding — field access")
{
    SUBCASE("a.b -> Field(Name a, 'b')")
    {
        auto e = ret("a.b");
        REQUIRE(field(*e) != nullptr);
        REQUIRE(is_name(*field_obj(*e), "a"));
        REQUIRE(*field_name(*e) == "b");
    }
    SUBCASE("a.b.c -> Field(Field(a,b), 'c') — left-nested")
    {
        auto e = ret("a.b.c");
        REQUIRE(field(*e) != nullptr);
        REQUIRE(*field_name(*e) == "c");
        auto* inner = field_obj(*e);
        REQUIRE(field(*inner) != nullptr);
        REQUIRE(*field_name(*inner) == "b");
        REQUIRE(is_name(*field_obj(*inner), "a"));
    }
}

TEST_CASE("parser: suffix folding — index access")
{
    SUBCASE("a[b] -> Index(Name a, b)")
    {
        auto e = ret("a[b]");
        REQUIRE(index(*e) != nullptr);
        REQUIRE(is_name(*index_obj(*e), "a"));
        REQUIRE(is_name(*index_key(*e), "b"));
    }
    SUBCASE("a[b][c] -> Index(Index(a,b), c) — left-nested")
    {
        auto e = ret("a[b][c]");
        REQUIRE(index(*e) != nullptr);
        auto* inner = index_obj(*e);
        REQUIRE(index(*inner) != nullptr);
        REQUIRE(is_name(*index_obj(*inner), "a"));
        REQUIRE(is_name(*index_key(*inner), "b"));
        REQUIRE(is_name(*index_key(*e), "c"));
    }
    SUBCASE("a.b[c] -> Index(Field(a,b), c) — mixed")
    {
        auto e = ret("a.b[c]");
        REQUIRE(index(*e) != nullptr);
        REQUIRE(field(*index_obj(*e)) != nullptr);
        REQUIRE(is_name(*index_key(*e), "c"));
    }
}

TEST_CASE("parser: suffix folding — calls")
{
    SUBCASE("f() -> Call(Name f, [])")
    {
        auto e = ret("f()");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(is_name(*call_func(*e), "f"));
        REQUIRE(call_args(*e)->empty());
    }
    SUBCASE("f(a) -> Call(f, [a])")
    {
        auto e = ret("f(a)");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(call_args(*e)->size() == 1);
        REQUIRE(is_name(*raw((*call_args(*e))[0]), "a"));
    }
    SUBCASE("f()() -> Call(Call(f,[]), []) — left-nested")
    {
        auto e = ret("f()()");
        REQUIRE(call(*e) != nullptr);
        auto* inner = call_func(*e);
        REQUIRE(call(*inner) != nullptr);
        REQUIRE(is_name(*call_func(*inner), "f"));
    }
    SUBCASE("f(a)(b) -> Call(Call(f,[a]), [b])")
    {
        auto e = ret("f(a)(b)");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(call_args(*e)->size() == 1);
        REQUIRE(is_name(*raw((*call_args(*e))[0]), "b"));
        auto* inner = call_func(*e);
        REQUIRE(call(*inner) != nullptr);
        REQUIRE(call_args(*inner)->size() == 1);
        REQUIRE(is_name(*raw((*call_args(*inner))[0]), "a"));
    }
}

TEST_CASE("parser: suffix folding — method calls")
{
    SUBCASE("o:m() -> MethodCall(o, 'm', [])")
    {
        auto e = ret("o:m()");
        REQUIRE(methodcall(*e) != nullptr);
        REQUIRE(is_name(*methodcall_obj(*e), "o"));
        REQUIRE(*methodcall_name(*e) == "m");
        REQUIRE(methodcall_args(*e)->empty());
    }
    SUBCASE("o:m(a)(b) -> Call(MethodCall(o,'m',[a]), [b])")
    {
        auto e = ret("o:m(a)(b)");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(call_args(*e)->size() == 1);
        REQUIRE(is_name(*raw((*call_args(*e))[0]), "b"));
        auto* mc = call_func(*e);
        REQUIRE(methodcall(*mc) != nullptr);
        REQUIRE(*methodcall_name(*mc) == "m");
        REQUIRE(methodcall_args(*mc)->size() == 1);
        REQUIRE(is_name(*raw((*methodcall_args(*mc))[0]), "a"));
    }
    SUBCASE("mixed chain: a.b.c:d(e) -> MethodCall(Field(Field(a,b),c),'d',[e])")
    {
        auto e = ret("a.b.c:d(e)");
        REQUIRE(methodcall(*e) != nullptr);
        REQUIRE(*methodcall_name(*e) == "d");
        REQUIRE(methodcall_args(*e)->size() == 1);
        REQUIRE(is_name(*raw((*methodcall_args(*e))[0]), "e"));
        // obj is a.b.c left-nested as Field(Field(Name a, b), c)
        auto* f1 = methodcall_obj(*e);
        REQUIRE(*field_name(*f1) == "c");
        auto* f2 = field_obj(*f1);
        REQUIRE(*field_name(*f2) == "b");
        REQUIRE(is_name(*field_obj(*f2), "a"));
    }
}

TEST_CASE("parser: parenthesized expressions are preserved")
{
    // Parens are deliberately NOT collapsed: (f()) truncates multires while
    // f() does not. The tree must keep the Paren node.
    SUBCASE("(f)() -> Call(Paren(f), [])")
    {
        auto e = ret("(f)()");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(paren(*call_func(*e)) != nullptr);
        REQUIRE(is_name(*paren_inner(*call_func(*e)), "f"));
    }
    SUBCASE("(x).y -> Field(Paren(x), 'y')")
    {
        auto e = ret("(x).y");
        REQUIRE(field(*e) != nullptr);
        REQUIRE(*field_name(*e) == "y");
        REQUIRE(paren(*field_obj(*e)) != nullptr);
        REQUIRE(is_name(*paren_inner(*field_obj(*e)), "x"));
    }
    SUBCASE("plain paren in arithmetic: (1+2)*3")
    {
        auto e = ret("(1+2)*3");
        REQUIRE(is_binop(*e, BinOpKind::Mul));
        REQUIRE(paren(*binop_lhs(*e)) != nullptr);
        REQUIRE(is_binop(*paren_inner(*binop_lhs(*e)), BinOpKind::Add));
    }
}

TEST_CASE("parser: suffix as lvalue (assignment target)")
{
    SUBCASE("t.k = v")
    {
        auto s = one_stat("t.k = v");
        REQUIRE(assign(*s) != nullptr);
        REQUIRE(assign_targets(*s)->size() == 1);
        REQUIRE(assign_values(*s)->size() == 1);
        auto* tgt = raw((*assign_targets(*s))[0]);
        REQUIRE(field(*tgt) != nullptr);
        REQUIRE(*field_name(*tgt) == "k");
        REQUIRE(is_name(*field_obj(*tgt), "t"));
        REQUIRE(is_name(*raw((*assign_values(*s))[0]), "v"));
    }
    SUBCASE("a[i], b.j = 1, 2 — multi-assign with index+field targets")
    {
        auto s = one_stat("a[i], b.j = 1, 2");
        REQUIRE(assign(*s) != nullptr);
        REQUIRE(assign_targets(*s)->size() == 2);
        REQUIRE(assign_values(*s)->size() == 2);
        REQUIRE(index(*raw((*assign_targets(*s))[0])) != nullptr);
        REQUIRE(field(*raw((*assign_targets(*s))[1])) != nullptr);
    }
}

TEST_CASE("parser: call-argument sugar")
{
    SUBCASE("f \"s\" -> Call(f, [StrLit])")
    {
        auto e = ret("f \"s\"");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(call_args(*e)->size() == 1);
        REQUIRE(strlit(*raw((*call_args(*e))[0])) != nullptr);
    }
    SUBCASE("f {1,2} -> Call(f, [TableCtor])")
    {
        auto e = ret("f {1, 2}");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(call_args(*e)->size() == 1);
        REQUIRE(tablector(*raw((*call_args(*e))[0])) != nullptr);
    }
    SUBCASE("f [[blk]] -> Call(f, [StrLit]) — long-bracket string arg")
    {
        auto e = ret("f [[blk]]");
        REQUIRE(call(*e) != nullptr);
        REQUIRE(call_args(*e)->size() == 1);
        REQUIRE(strlit(*raw((*call_args(*e))[0])) != nullptr);
    }
}

// ===========================================================================
// D. Statement structure — child attachment.
// ===========================================================================
TEST_CASE("parser: if / elseif / else — branch bodies attach correctly")
{
    SUBCASE("branch conditions and bodies")
    {
        auto s = one_stat(
            "if x < 1 then return 10 elseif x < 2 then return 20 else return 30 end");
        REQUIRE(if_stat(*s) != nullptr);
        auto& brs = *if_branches(*s);
        REQUIRE(brs.size() == 2);
        // branch 0 cond is Lt, body block holds a Return
        REQUIRE(is_binop(*raw(brs[0].first), BinOpKind::Lt));
        auto* body0 = as<Block>(*raw(brs[0].second));
        REQUIRE(body0 != nullptr);
        REQUIRE(body0->stmts.size() == 1);
        REQUIRE(return_stat(*raw(body0->stmts[0])) != nullptr);
        // branch 1 cond is Lt too
        REQUIRE(is_binop(*raw(brs[1].first), BinOpKind::Lt));
        // else present
        REQUIRE(if_else(*s) != nullptr);
        auto* elsebody = as<Block>(*raw(*if_stat(*s)->else_body));
        REQUIRE(elsebody != nullptr);
        REQUIRE(elsebody->stmts.size() == 1);
        REQUIRE(return_stat(*raw(elsebody->stmts[0])) != nullptr);
    }
    SUBCASE("no else")
    {
        auto s = one_stat("if x then return 1 end");
        REQUIRE(if_stat(*s) != nullptr);
        REQUIRE(if_branches(*s)->size() == 1);
        REQUIRE(if_else(*s) == nullptr);
    }
}

TEST_CASE("parser: for loops")
{
    SUBCASE("numeric for, no step: for i=1,10 do end")
    {
        auto s = one_stat("for i = 1, 10 do end");
        REQUIRE(numfor(*s) != nullptr);
        REQUIRE(numfor(*s)->var == "i");
        REQUIRE(intlit(*raw(numfor(*s)->init)) != nullptr);
        REQUIRE(intlit(*raw(numfor(*s)->limit)) != nullptr);
        REQUIRE(!numfor(*s)->step.has_value());
    }
    SUBCASE("numeric for, with step: for i=1,10,2 do end")
    {
        auto s = one_stat("for i = 1, 10, 2 do end");
        REQUIRE(numfor(*s) != nullptr);
        REQUIRE(numfor(*s)->step.has_value());
        REQUIRE(intlit(*raw(*numfor(*s)->step)) != nullptr);
    }
    SUBCASE("generic for: for k,v in pairs(t) do end")
    {
        auto s = one_stat("for k, v in pairs(t) do end");
        REQUIRE(genfor(*s) != nullptr);
        REQUIRE(genfor(*s)->names == std::vector<std::string>{"k", "v"});
        REQUIRE(genfor(*s)->exprs.size() == 1);
        // the single expr is a Call(pairs, [t])
        auto* e = raw(genfor(*s)->exprs[0]);
        REQUIRE(call(*e) != nullptr);
        REQUIRE(is_name(*call_func(*e), "pairs"));
    }
}

TEST_CASE("parser: local declarations")
{
    SUBCASE("local x = 1")
    {
        auto s = one_stat("local x = 1");
        REQUIRE(localdecl(*s) != nullptr);
        REQUIRE(localdecl(*s)->names == std::vector<std::string>{"x"});
        REQUIRE(localdecl(*s)->attribs.size() == 1);
        REQUIRE(localdecl(*s)->attribs[0] == Attrib::None);
        REQUIRE(localdecl(*s)->values.has_value());
        REQUIRE(localdecl(*s)->values->size() == 1);
    }
    SUBCASE("local x, y = 1, 2")
    {
        auto s = one_stat("local x, y = 1, 2");
        REQUIRE(localdecl(*s)->names == std::vector<std::string>{"x", "y"});
        REQUIRE(localdecl(*s)->values->size() == 2);
    }
    SUBCASE("local x — no values")
    {
        auto s = one_stat("local x");
        REQUIRE(localdecl(*s) != nullptr);
        REQUIRE(!localdecl(*s)->values.has_value());
    }
    SUBCASE("local x <const> = 1")
    {
        auto s = one_stat("local x <const> = 1");
        REQUIRE(localdecl(*s)->attribs[0] == Attrib::Const);
    }
    SUBCASE("local x <close>")
    {
        auto s = one_stat("local x <close>");
        REQUIRE(localdecl(*s)->attribs[0] == Attrib::Close);
        REQUIRE(!localdecl(*s)->values.has_value());
    }
}

TEST_CASE("parser: function statements")
{
    SUBCASE("local function f() end")
    {
        auto s = one_stat("local function f() end");
        REQUIRE(localfunction(*s) != nullptr);
        REQUIRE(localfunction(*s)->name == "f");
        REQUIRE(localfunction(*s)->body.params.empty());
    }
    SUBCASE("function a.b.c:d() end — dotted path + method")
    {
        auto s = one_stat("function a.b.c:d() end");
        REQUIRE(funcstat(*s) != nullptr);
        REQUIRE(funcstat(*s)->name.fields ==
                std::vector<std::string>{"a", "b", "c"});
        REQUIRE(funcstat(*s)->name.method.has_value());
        REQUIRE(*funcstat(*s)->name.method == "d");
    }
    SUBCASE("function t.f() end — no method")
    {
        auto s = one_stat("function t.f() end");
        REQUIRE(funcstat(*s) != nullptr);
        REQUIRE(funcstat(*s)->name.fields == std::vector<std::string>{"t", "f"});
        REQUIRE(!funcstat(*s)->name.method.has_value());
    }
}

TEST_CASE("parser: while / repeat / do")
{
    SUBCASE("while: cond then body")
    {
        auto s = one_stat("while x > 0 do x = x - 1 end");
        REQUIRE(while_stat(*s) != nullptr);
        REQUIRE(is_binop(*while_cond(*s), BinOpKind::Gt));
        auto* body = as<Block>(*raw(while_stat(*s)->body));
        REQUIRE(body != nullptr);
        REQUIRE(body->stmts.size() == 1);
        REQUIRE(assign(*raw(body->stmts[0])) != nullptr);
    }
    SUBCASE("repeat: body then cond (order matters)")
    {
        auto s = one_stat("repeat x = x + 1 until x >= 10");
        REQUIRE(repeat_stat(*s) != nullptr);
        // body is a block with one assign
        auto* body = as<Block>(*raw(repeat_stat(*s)->body));
        REQUIRE(body != nullptr);
        REQUIRE(body->stmts.size() == 1);
        REQUIRE(assign(*raw(body->stmts[0])) != nullptr);
        // cond is the Ge
        REQUIRE(is_binop(*repeat_cond(*s), BinOpKind::Ge));
    }
    SUBCASE("do block")
    {
        auto s = one_stat("do x = 1 end");
        REQUIRE(do_stat(*s) != nullptr);
        auto* body = as<Block>(*raw(do_stat(*s)->body));
        REQUIRE(body != nullptr);
        REQUIRE(body->stmts.size() == 1);
    }
}

TEST_CASE("parser: goto / label / break / return")
{
    SUBCASE("goto + label")
    {
        auto root = parse_ok("goto done ::done::");
        REQUIRE(goto_stat(*stmt_at(root, 0)) != nullptr);
        REQUIRE(goto_stat(*stmt_at(root, 0))->label == "done");
        REQUIRE(label_stat(*stmt_at(root, 1)) != nullptr);
        REQUIRE(label_stat(*stmt_at(root, 1))->name == "done");
    }
    SUBCASE("break")
    {
        auto s = one_stat("break");
        REQUIRE(break_stat(*s) != nullptr);
    }
    SUBCASE("return with values")
    {
        auto root = parse_ok("return a, b, c");
        auto* r = return_stat(*stmt_at(root, 0));
        REQUIRE(r != nullptr);
        REQUIRE(return_values(*stmt_at(root, 0))->size() == 3);
    }
    SUBCASE("bare return")
    {
        auto root = parse_ok("do return end");
        auto* doblk = as<Do>(*stmt_at(root, 0));
        REQUIRE(doblk != nullptr);
        auto* body = as<Block>(*raw(doblk->body));
        REQUIRE(body != nullptr);
        REQUIRE(body->stmts.size() == 1);
        auto* r = return_stat(*raw(body->stmts[0]));
        REQUIRE(r != nullptr);
        REQUIRE(!r->values.has_value());
    }
}

TEST_CASE("parser: call as statement (CallStat)")
{
    SUBCASE("f() as a statement")
    {
        auto s = one_stat("f()");
        REQUIRE(callstat(*s) != nullptr);
        REQUIRE(call(*callstat_call(*s)) != nullptr);
    }
    SUBCASE("o:m(a) as a statement")
    {
        auto s = one_stat("o:m(a)");
        REQUIRE(callstat(*s) != nullptr);
        REQUIRE(methodcall(*callstat_call(*s)) != nullptr);
    }
}

// ===========================================================================
// E. Function parameters
// ===========================================================================
TEST_CASE("parser: function parameter lists")
{
    auto check_params = [](std::string_view src, std::size_t n_names, bool has_dots) {
        auto e = ret(src);
        auto* fd = funcdef(*e);
        REQUIRE(fd != nullptr);
        auto& params = fd->body.params;
        REQUIRE(params.size() == n_names + (has_dots ? 1 : 0));
        std::size_t dots = 0;
        for (auto& p : params)
            if (p.kind == Param::Kind::Vararg) ++dots;
        REQUIRE(dots == (has_dots ? 1u : 0u));
    };
    SUBCASE("named only") { check_params("function(a, b) end", 2, false); }
    SUBCASE("named + vararg") { check_params("function(a, b, ...) end", 2, true); }
    SUBCASE("vararg only") { check_params("function(...) end", 0, true); }
    SUBCASE("no params") { check_params("function() end", 0, false); }
}

// ===========================================================================
// F. Table constructors
// ===========================================================================
TEST_CASE("parser: table constructor field forms")
{
    SUBCASE("positional: {1, 2, 3}")
    {
        auto e = ret("{1, 2, 3}");
        auto* tc = tablector(*e);
        REQUIRE(tc != nullptr);
        REQUIRE(tc->fields.size() == 3);
        for (auto& f : tc->fields)
            REQUIRE(f.kind == FieldEntry::Kind::Positional);
    }
    SUBCASE("named: {x=1, y=2}")
    {
        auto e = ret("{x = 1, y = 2}");
        auto* tc = tablector(*e);
        REQUIRE(tc->fields.size() == 2);
        REQUIRE(tc->fields[0].kind == FieldEntry::Kind::Named);
        REQUIRE(tc->fields[0].name == "x");
        REQUIRE(tc->fields[1].kind == FieldEntry::Kind::Named);
        REQUIRE(tc->fields[1].name == "y");
    }
    SUBCASE("bracketed: {[k] = v}")
    {
        auto e = ret("{[k] = v}");
        auto* tc = tablector(*e);
        REQUIRE(tc->fields.size() == 1);
        REQUIRE(tc->fields[0].kind == FieldEntry::Kind::Bracketed);
        REQUIRE(is_name(*raw(tc->fields[0].key), "k"));
        REQUIRE(is_name(*raw(tc->fields[0].value), "v"));
    }
    SUBCASE("mixed separators: {1; 2, 3} — ; and , both allowed, trailing ok")
    {
        auto e = ret("{1; 2, 3,}");
        auto* tc = tablector(*e);
        REQUIRE(tc->fields.size() == 3);
    }
    SUBCASE("empty: {}")
    {
        auto e = ret("{}");
        auto* tc = tablector(*e);
        REQUIRE(tc != nullptr);
        REQUIRE(tc->fields.empty());
    }
    SUBCASE("last positional with trailing semicolon: {1, 2;}")
    {
        auto e = ret("{1, 2;}");
        auto* tc = tablector(*e);
        REQUIRE(tc->fields.size() == 2);
    }
}

// ===========================================================================
// G. Source ranges — prove nodes carry correct byte spans.
// ===========================================================================
TEST_CASE("parser: source ranges")
{
    SUBCASE("a bare Name span")
    {
        // "return a" — the Name 'a' starts at byte 7, ends at 8 (exclusive).
        auto e = ret("a");
        auto* nm = name(*e);
        REQUIRE(nm != nullptr);
        REQUIRE(nm->start == 7u);
        REQUIRE(nm->end == 8u);
    }
    SUBCASE("int literal span")
    {
        // source is "return 42"; the literal '42' is at bytes 7..9.
        auto root = parse_ok("return 42");
        auto* r = return_stat(*stmt_at(root, 0));
        auto* il = intlit(*raw((*r->values)[0]));
        REQUIRE(il != nullptr);
        REQUIRE(il->start == 7u);
        REQUIRE(il->end == 9u);
    }
    SUBCASE("operator span covers both operands")
    {
        // ret() parses "return a + b"; 'a' is at byte 7, 'b' ends at byte 12.
        auto e = ret("a + b");
        auto* b = binop(*e);
        REQUIRE(b != nullptr);
        REQUIRE(b->start == 7u);   // 'a'
        REQUIRE(b->end == 12u);    // end of 'b'
    }
}

// ===========================================================================
// H. Multi-statement programs — prove the block holds them in order.
// ===========================================================================
TEST_CASE("parser: block holds statements in source order")
{
    std::string src = "local x = 1\nx = x + 1\nprint(x)\n";
    auto root = parse_ok(src);
    REQUIRE(stmts_of(root)->size() == 3);
    REQUIRE(localdecl(*stmt_at(root, 0)) != nullptr);
    REQUIRE(assign(*stmt_at(root, 1)) != nullptr);
    REQUIRE(callstat(*stmt_at(root, 2)) != nullptr);
}

TEST_CASE("parser: nested blocks via do/if expose their inner statements")
{
    auto s = one_stat("do local x = 1 return x end");
    auto* d = do_stat(*s);
    REQUIRE(d != nullptr);
    auto* body = as<Block>(*raw(d->body));
    REQUIRE(body != nullptr);
    REQUIRE(body->stmts.size() == 2);
    REQUIRE(localdecl(*raw(body->stmts[0])) != nullptr);
    REQUIRE(return_stat(*raw(body->stmts[1])) != nullptr);
}
