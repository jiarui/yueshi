#pragma once

// yueshi Lua 5.4 parser grammar (token-level), typed-fold edition.
//
// Built by Parser (parser.h); each Parser owns a fresh Grammar.
//
// DESIGN (transcribes the official Lua 5.4 EBNF from §9, with two concessions
// to peglib's typed-fold PEG model):
//
//   1. LEFT RECURSION IS NATIVE. peglib implements the Warth/Okasaki seed-grow
//      algorithm (NonTerminal.h:193-229), so `var ::= var '.' Name | Name` and
//      the var/functioncall/prefixexp mutual-recursion triangle are written
//      verbatim — NO suffix-loop expansion. Each left-recursive production is
//      its own rule so its set_action's argument shape is fixed (a single rule
//      in an alternation would force all branches to share one result type,
//      which the differently-shaped productions violate).
//
//   2. BINARY OPERATORS use a PRECEDENCE LADDER, NOT left recursion. peglib's
//      left recursion is precedence-unaware (TODO.md:625): `exp = exp '+' exp`
//      parses but yields a wrong-associativity AST. So §3.4.8's 14 levels are
//      encoded as `opd >> *((op_alt) >> opd)` (left-assoc) or
//      `opd >> -(op_alt >> same_rule)` (right-assoc), matching the canonical
//      shape in peglib's own typed_action_test.cpp:159.
//
// API (peglib a4829d2, two-phase typed fold):
//   - Actions attach via the RuleHandle from `g["r"] = body`:
//         auto h = (g["r"] = body);  h.set_action([](Ctx&, Span, ...) {...});
//   - The action's positional args are the body's non-void results, in order.
//     void terminals (g.terminal) are FILTERED OUT; g.token survives as a Token.
//   - `Span` holds TOKEN indices; recover source bytes via ctx.at(sp.start).
//   - `*(op >> opd)` → vector<tuple<op, opd>>; `*x`/`+x` → vector<T>;
//     `-x` → optional<T> (or void if x is void); `A | B` → common type T.
//   - parse_ast (not parse_tree) runs actions and returns optional<NodeType>.

#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "peglib.h"
#include "lua/ast.h"
#include "lua/lex.h"

namespace ys::lua::parserconv {

// Forward decl (defined after make_grammar); used by the attnamelist action.
inline Attrib attrib_of(const std::string& s);

inline peg::Grammar<Token, AstNode> make_grammar()
{
    using namespace peg;
    using Ctx = Context<Token, AstNode>;
    using NodePtr = Ctx::ParseTreeNodePtr;

    Grammar<Token, AstNode> g;

    // -----------------------------------------------------------------------
    // Terminal helpers.
    //   tok_id/tok_char  → g.terminal: VOID (filtered). Structural tokens the
    //     action never inspects: keywords, () [] {} , ; EOS.
    //   op_tok_id/char   → g.token: the matched Token survives into the action.
    //     Used for operators the action must discriminate (BinOpKind/UnOpKind).
    // -----------------------------------------------------------------------
    using IDType = Token::TokenIDType;
    auto tok_id = [&g](TokenID id) {
        return g.terminal([id](const Token& t) {
            return t.id == static_cast<IDType>(id);
        });
    };
    auto tok_char = [&g](int ch) {
        return g.terminal([ch](const Token& t) { return t.id == ch; });
    };
    auto op_tok_id = [&g](TokenID id) {
        return g.token([id](const Token& t) {
            return t.id == static_cast<IDType>(id);
        });
    };
    auto op_tok_char = [&g](int ch) {
        return g.token([ch](const Token& t) { return t.id == ch; });
    };

    // Source byte span of a match (Span carries token indices).
    auto bstart = [](Ctx& c, Span sp) -> std::size_t { return c.at(sp.start).start; };
    auto bend = [](Ctx& c, Span sp) -> std::size_t {
        std::size_t last = sp.end > 0 ? sp.end - 1 : 0;
        return c.at(last).end;
    };

    // =======================================================================
    // PRIMARY EXPRESSIONS — literals, Name, prefixexp/var/functioncall (§9).
    // The var/functioncall/prefixexp triangle is transcribed verbatim: peglib
    // handles the mutual left recursion via seed-grow. Each left-recursive
    // production is a separate rule so its action arg shape is fixed.
    // =======================================================================

    // --- Literals (void-terminal leaves: action reads ctx.at(sp.start)) ---
    {
        auto h = (g["nil"] = tok_id(TokenID::TK_NIL));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            return Nil{bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["true_"] = tok_id(TokenID::TK_TRUE));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            return True{bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["false_"] = tok_id(TokenID::TK_FALSE));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            return False{bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["dots"] = tok_id(TokenID::TK_DOTS));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            return Vararg{bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["number"] = tok_id(TokenID::TK_INT) | tok_id(TokenID::TK_FLT));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            const Token& t = c.at(sp.start);
            std::size_t s = bstart(c, sp), e = bend(c, sp);
            if (t.id == static_cast<IDType>(TokenID::TK_INT))
                return IntLit{std::get<long long>(t.info), s, e};
            return FltLit{std::get<double>(t.info), s, e};
        });
    }
    {
        auto h = (g["literal_string"] = tok_id(TokenID::TK_STRING));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            const Token& t = c.at(sp.start);
            return StrLit{std::get<std::string>(t.info), bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["name_token"] = tok_id(TokenID::TK_NAME));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            const Token& t = c.at(sp.start);
            return Name{std::get<std::string>(t.info), bstart(c, sp), bend(c, sp)};
        });
    }

    // --- explist / namelist / parlist (§9) ---
    // Each is `head {',' tail}` with a void ','. The repetition therefore
    // collapses to vector<T> (single non-void child per iteration). We carry
    // the list in a Block node (reused as a generic stmts/Box vector), the
    // convention every list action below follows.
    {
        auto h = (g["explist"] = g["exp"] >> *(tok_char(',') >> g["exp"]));
        h.set_action([](Ctx&, Span, AstNode first, std::vector<AstNode> rest) -> AstNode {
            Block list;
            list.stmts.emplace_back(Box{std::move(first)});
            for (auto& r : rest) list.stmts.emplace_back(Box{std::move(r)});
            return list;
        });
    }
    {
        auto h = (g["namelist"] = g["name_token"] >> *(tok_char(',') >> g["name_token"]));
        h.set_action([](Ctx&, Span, AstNode first, std::vector<AstNode> rest) -> AstNode {
            Block list;
            list.stmts.emplace_back(Box{std::move(first)});
            for (auto& r : rest) list.stmts.emplace_back(Box{std::move(r)});
            return list;
        });
    }
    {
        // parlist_names = namelist [',' '...']. ',' void → optional<AstNode>.
        auto h = (g["parlist_names"] = g["namelist"] >> -(tok_char(',') >> g["dots"]));
        h.set_action([](Ctx&, Span, AstNode names, std::optional<AstNode> vararg) -> AstNode {
            Block list = std::move(get<Block>(names));
            if (vararg) list.stmts.emplace_back(Box{std::move(*vararg)});
            return list;
        });
    }
    {
        auto h = (g["parlist_dots"] = g["dots"]);
        h.set_action([](Ctx&, Span, AstNode d) -> AstNode {
            Block list;
            list.stmts.emplace_back(Box{std::move(d)});
            return list;
        });
    }
    {
        auto h = (g["parlist"] = g["parlist_names"] | g["parlist_dots"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }

    // =======================================================================
    // var / functioncall / prefixexp — the §9 left-recursive triangle.
    //
    //   var           ::= Name | var '.' Name | var '[' exp ']'
    //   functioncall  ::= var args | var ':' Name args
    //   args          ::= '(' [explist] ')' | tableconstructor | literal_string
    //   prefixexp     ::= var | functioncall | '(' exp ')'
    //
    // NOTE on the original BNF: it writes `prefixexp '.' Name` etc., but since
    // `prefixexp ::= var | functioncall | '(' exp ')'`, a '.' suffix can only
    // legitimately apply to a var (a functioncall followed by '.' is a syntax
    // error, and '(exp).name' reduces to var). The seed-grow converges to the
    // var-centred form below, which is equivalent and what the AST needs.
    // =======================================================================

    // tableconstructor + fields (defined before args, which can be a table).
    {
        auto h = (g["field_named"] = g["name_token"] >> tok_char('=') >> g["exp"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm, AstNode val) -> AstNode {
            FieldEntry f;
            f.kind = FieldEntry::Kind::Named;
            f.name = get<Name>(nm).name;
            f.value = Box{std::move(val)};
            f.start = bstart(c, sp);
            f.end = bend(c, sp);
            return f;
        });
    }
    {
        auto h = (g["field_bracketed"] =
                      tok_char('[') >> g["exp"] >> tok_char(']') >> tok_char('=') >> g["exp"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode key, AstNode val) -> AstNode {
            FieldEntry f;
            f.kind = FieldEntry::Kind::Bracketed;
            f.key = Box{std::move(key)};
            f.value = Box{std::move(val)};
            f.start = bstart(c, sp);
            f.end = bend(c, sp);
            return f;
        });
    }
    {
        auto h = (g["field_positional"] = g["exp"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode val) -> AstNode {
            FieldEntry f;
            f.kind = FieldEntry::Kind::Positional;
            f.value = Box{std::move(val)};
            f.start = bstart(c, sp);
            f.end = bend(c, sp);
            return f;
        });
    }
    {
        // Ordered: bracketed/named before positional so `x = 1` and `[k]=v`
        // are not misread as positional exps.
        auto h = (g["field"] = g["field_bracketed"] | g["field_named"] | g["field_positional"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }
    {
        auto fieldsep = tok_char(',') | tok_char(';');
        // field {fieldsep field} [fieldsep]. fieldsep void → repetition is
        // vector<AstNode>; trailing optional fieldsep is opt_of<void> = void.
        auto h = (g["fieldlist"] = g["field"] >> *(fieldsep >> g["field"]) >> -fieldsep);
        h.set_action([](Ctx&, Span, AstNode first, std::vector<AstNode> rest) -> AstNode {
            Block list;
            list.stmts.emplace_back(Box{std::move(first)});
            for (auto& r : rest) list.stmts.emplace_back(Box{std::move(r)});
            return list;
        });
    }
    {
        auto h = (g["tableconstructor"] = tok_char('{') >> -g["fieldlist"] >> tok_char('}'));
        h.set_action([bstart, bend](Ctx& c, Span sp, std::optional<AstNode> listnode) -> AstNode {
            TableCtor t;
            if (listnode) {
                for (auto& box : get<Block>(*listnode).stmts)
                    t.fields.push_back(std::move(get<FieldEntry>(*box)));
            }
            t.start = bstart(c, sp);
            t.end = bend(c, sp);
            return t;
        });
    }

    // args (§9).
    {
        // '(' [explist] ')' → Call. '(' ')' void; -explist → optional<AstNode>.
        auto h = (g["args_paren"] = tok_char('(') >> -g["explist"] >> tok_char(')'));
        h.set_action([bstart, bend](Ctx& c, Span sp, std::optional<AstNode> args) -> AstNode {
            Call node;
            if (args)
                for (auto& box : get<Block>(*args).stmts) node.args.emplace_back(std::move(box));
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // args = paren | table | string. All branches yield AstNode (Call/
        // TableCtor/StrLit). The alternation's common type is AstNode.
        auto h = (g["args"] = g["args_paren"] | g["tableconstructor"] | g["literal_string"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }

    // var / functioncall / prefixexp — the §9 left-recursive triangle.
    //
    //   prefixexp    ::= var | functioncall | '(' exp ')'
    //   var          ::= Name | prefixexp '[' exp ']' | prefixexp '.' Name
    //   functioncall ::= prefixexp args | prefixexp ':' Name args
    //
    // Transcribed VERBATIM: the left recursion routes through `prefixexp`
    // (NOT directly through var). Each production is its own rule so its
    // action arg shape is fixed; the alternations at prefixexp/var/functioncall
    // unify to AstNode.
    //
    // ORDERING within alternations matters for peglib's seed-grow under
    // ordered-choice semantics. The rule (verified empirically across all
    // permutations of each alternation's branches):
    //
    //   • In `var`, the recursive suffixes (var_field, var_index) MUST precede
    //     the base case (var_name). If base is first, it matches the leading
    //     Name of the current seed every iteration and short-circuits before
    //     the suffix is re-evaluated → no growth. When suffixes come first,
    //     each growth iteration tries them against the grown seed and extends.
    //
    //   • In `prefixexp`, var MUST precede functioncall (the opposite of the
    //     suffix-first rule above). peglib's lr_token_triangle_test argues for
    //     functioncall-first, but its triangle is a stripped-down model that
    //     omits yueshi's precedence ladder and rule actions; in the REAL
    //     grammar functioncall-first plants a zero-width seed and nothing grows
    //     (see the comment on the prefixexp rule below). With var-first the
    //     seed grows for every suffix form, and functioncall is reached
    //     directly from stat_call / simple_exp instead of growing through
    //     prefixexp.
    {
        auto h = (g["var_name"] = g["name_token"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }
    {
        // prefixexp '.' Name → Field{obj, name}.
        auto h = (g["var_field"] = g["prefixexp"] >> tok_char('.') >> g["name_token"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode obj, AstNode nm) -> AstNode {
            Field node;
            node.obj = Box{std::move(obj)};
            node.name = get<Name>(nm).name;
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // prefixexp '[' exp ']' → Index{obj, key}.
        auto h = (g["var_index"] = g["prefixexp"] >> tok_char('[') >> g["exp"] >> tok_char(']'));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode obj, AstNode key) -> AstNode {
            Index node;
            node.obj = Box{std::move(obj)};
            node.key = Box{std::move(key)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // var ::= prefixexp '.' Name | prefixexp '[' exp ']' | Name.
        // Recursive suffixes BEFORE base case so the seed-grow extends.
        auto h = (g["var"] = g["var_field"] | g["var_index"] | g["var_name"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }

    {
        // prefixexp args → Call{func, args}.
        auto h = (g["call_plain"] = g["prefixexp"] >> g["args"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode fn, AstNode argsnode) -> AstNode {
            Call node;
            node.func = Box{std::move(fn)};
            if (holds<Call>(argsnode)) {
                node.args = std::move(get<Call>(argsnode).args);
            } else {
                node.args.emplace_back(Box{std::move(argsnode)});
            }
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // prefixexp ':' Name args → MethodCall{obj, method, args}.
        auto h = (g["call_method"] = g["prefixexp"] >> tok_char(':') >> g["name_token"] >> g["args"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode obj, AstNode nm, AstNode argsnode)
                         -> AstNode {
            MethodCall node;
            node.obj = Box{std::move(obj)};
            node.method = get<Name>(nm).name;
            if (holds<Call>(argsnode)) {
                node.args = std::move(get<Call>(argsnode).args);
            } else {
                node.args.emplace_back(Box{std::move(argsnode)});
            }
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // Method-call postfix used by `functioncall` below: ':' Name args,
        // folded to a MethodCall whose obj slot is filled by the caller (the
        // accumulated prefix). Factored into its own rule so the alternation
        // `args | call_method_tail` has a uniform AstNode result type
        // (args → Call/TableCtor/StrLit; tail → MethodCall) — peglib's
        // alternation requires all branches to share a result type.
        auto h = (g["call_method_tail"] =
                      tok_char(':') >> g["name_token"] >> g["args"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm, AstNode argsnode)
                         -> AstNode {
            MethodCall node;
            node.method = get<Name>(nm).name;
            if (holds<Call>(argsnode)) {
                node.args = std::move(get<Call>(argsnode).args);
            } else {
                node.args.emplace_back(Box{std::move(argsnode)});
            }
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // functioncall ::= (call_plain | call_method) { args | ':' Name args }.
        // Each postfix wraps the running result into another call, handling
        // chained calls like f()()() without seed-grow growth (flat repetition
        // in the body — functioncall's producer stamps the sequence node).
        // Postfix results: args_paren → Call; tableconstructor/literal_string
        // → wrapped in Call by this action; ':Name args' → MethodCall.
        auto h = (g["functioncall"] =
                      (g["call_plain"] | g["call_method"]) >>
                      *(g["args"] | g["call_method_tail"]));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode first,
                                     std::vector<AstNode> rest) -> AstNode {
            AstNode acc = std::move(first);
            for (auto& r : rest) {
                if (holds<Call>(r)) {
                    // args_paren → existing Call, append func
                    get<Call>(r).func = Box{std::move(acc)};
                    acc = std::move(r);
                } else if (holds<MethodCall>(r)) {
                    // ':Name args' → existing MethodCall, append obj
                    get<MethodCall>(r).obj = Box{std::move(acc)};
                    acc = std::move(r);
                } else {
                    // tableconstructor or literal_string → wrap in Call
                    Call node;
                    node.func = Box{std::move(acc)};
                    node.args.emplace_back(Box{std::move(r)});
                    node.start = bstart(c, sp);
                    node.end = bend(c, sp);
                    acc = std::move(node);
                }
            }
            return acc;
        });
    }

    // prefixexp ::= var | functioncall | '(' exp ')'.
    {
        auto h = (g["prefixexp_parens"] = tok_char('(') >> g["exp"] >> tok_char(')'));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode e) -> AstNode {
            Paren node;
            node.exp = Box{std::move(e)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // prefixexp ::= var | functioncall | '(' exp ')'.
        //
        // ORDERING: var MUST precede functioncall here. This is the OPPOSITE of
        // what peglib's lr_token_triangle_test documents as "correct", and the
        // reason is empirical, not theoretical: with functioncall-first, the
        // grow-loop plants a zero-width functioncall seed and every input
        // (including a.b, t[1], and bare calls) fails to grow — consumed==0
        // across the board (verified with the parser_probe harness). The
        // peglib test's triangle differs from yueshi's real grammar in two
        // load-bearing ways: (1) its `exp = prefixexp | digit` is flat, while
        // yueshi wraps prefixexp in a full precedence ladder (exp → or_exp →
        // … → unop_exp → pow_exp → simple_exp → prefixexp); (2) it carries no
        // set_action on functioncall/var, while yueshi does. Either difference
        // (most likely the precedence ladder re-entering the LR head) breaks
        // functioncall-first growth in the real grammar. With var-first the
        // seed grows correctly for all of a, a.b, a.b.c, t[1], f(), f(1,2,3),
        // o:m(42), f()() (parser_probe: consumed == size-1 in every case).
        // functioncall is instead reached directly from stat_call / simple_exp,
        // so it never needs to grow through prefixexp.
        auto h = (g["prefixexp"] = g["var"] | g["functioncall"] | g["prefixexp_parens"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }

    // =======================================================================
    // functiondef / funcbody (§9)
    //   functiondef ::= function funcbody
    //   funcbody    ::= '(' [parlist] ')' block end
    // =======================================================================
    {
        auto h = (g["funcbody"] =
                      tok_char('(') >> -g["parlist"] >> tok_char(')') >> g["block"] >> tok_id(TokenID::TK_END));
        h.set_action([bstart, bend](Ctx& c, Span sp, std::optional<AstNode> parlistnode,
                                    AstNode bodynode) -> AstNode {
            FuncBody fb;
            if (parlistnode) {
                for (auto& box : get<Block>(*parlistnode).stmts) {
                    Param p;
                    if (holds<Name>(*box)) {
                        p.kind = Param::Kind::Name;
                        p.name = get<Name>(*box).name;
                    } else {
                        p.kind = Param::Kind::Vararg;
                    }
                    p.start = box->start();
                    p.end = box->end();
                    fb.params.push_back(std::move(p));
                }
            }
            fb.body = Box{std::move(bodynode)};
            fb.start = bstart(c, sp);
            fb.end = bend(c, sp);
            return fb;
        });
    }
    {
        auto h = (g["functiondef"] = tok_id(TokenID::TK_FUNCTION) >> g["funcbody"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode bodynode) -> AstNode {
            FuncDef fd;
            fd.body = std::move(get<FuncBody>(bodynode));
            fd.start = bstart(c, sp);
            fd.end = bend(c, sp);
            return fd;
        });
    }

    // =======================================================================
    // EXPRESSIONS — precedence ladder (§3.4.8). Left-assoc via
    //   opd >> *((op_alt) >> opd)   →  F(Ctx,Span, AstNode, vector<tuple<Token,AstNode>>)
    // right-assoc via
    //   opd >> -(op_alt >> same)    →  F(Ctx,Span, AstNode, optional<tuple<Token,AstNode>>)
    // unop (level 11) sits BETWEEN mul(10) and pow(12): -2^2 == -4, 2^-2 == .25.
    // =======================================================================

    // simple_exp ::= nil | false | true | number | string | '...' | functiondef
    //                | tableconstructor | prefixexp   (§9 exp atoms)
    {
        auto h = (g["simple_exp"] =
                      g["nil"] | g["false_"] | g["true_"] | g["number"] | g["literal_string"] |
                      g["dots"] | g["functiondef"] | g["tableconstructor"] | g["prefixexp"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }

    // pow (level 12, RIGHT-assoc): simple_exp ('^' unop_exp)?.
    {
        auto h = (g["pow_exp"] = g["simple_exp"] >> -(op_tok_char('^') >> g["unop_exp"]));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode lhs,
                                    std::optional<std::tuple<Token, AstNode>> rhs) -> AstNode {
            if (!rhs) return lhs;
            BinOp b;
            b.op = BinOpKind::Pow;
            b.lhs = Box{std::move(lhs)};
            b.rhs = Box{std::move(std::get<1>(*rhs))};
            b.start = bstart(c, sp);
            b.end = bend(c, sp);
            return b;
        });
    }

    // unop (level 11, prefix, RIGHT-assoc): one-or-more unary ops then pow.
    //   unop_exp = *(unop_alt) pow_exp  →  F(Ctx,Span, vector<Token>, AstNode)
    // Each matched op token folds a UnOp around the running operand.
    {
        auto unop_alt = op_tok_char('-') | op_tok_id(TokenID::TK_NOT) |
                        op_tok_char('#') | op_tok_char('~');
        auto h = (g["unop_exp"] = *unop_alt >> g["pow_exp"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, std::vector<Token> ops,
                                    AstNode operand) -> AstNode {
            // Fold right-to-left so '--x' nests: Neg(Neg(x)).
            AstNode acc = std::move(operand);
            for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
                UnOp u;
                const Token& op = *it;
                if (op.id == '-') u.op = UnOpKind::Neg;
                else if (op.id == static_cast<IDType>(TokenID::TK_NOT)) u.op = UnOpKind::Not;
                else if (op.id == '#') u.op = UnOpKind::Len;
                else u.op = UnOpKind::BNot;  // '~'
                u.operand = Box{std::move(acc)};
                u.start = bstart(c, sp);
                u.end = bend(c, sp);
                acc = std::move(u);
            }
            return acc;
        });
    }

    // Left-assoc binary-level helper. Captures g/bstart/bend; op_alt and
    // kind_fn are taken by const ref (the `>>`/`*` combinators copy exprs in).
    auto bin_left = [&g, &bstart, &bend](const char* name, const char* operand,
                                         const auto& op_alt, const auto& kind_fn) {
        auto h = (g[name] = g[operand] >> *(op_alt >> g[operand]));
        h.set_action([bstart, bend, kind_fn](Ctx& c, Span sp, AstNode first,
                                             std::vector<std::tuple<Token, AstNode>> rest)
                         -> AstNode {
            AstNode acc = std::move(first);
            for (auto& [opt, rhs] : rest) {
                BinOp b;
                b.op = kind_fn(opt);
                b.lhs = Box{std::move(acc)};
                b.rhs = Box{std::move(rhs)};
                b.start = bstart(c, sp);
                b.end = bend(c, sp);
                acc = std::move(b);
            }
            return acc;
        });
    };

    // mul/div/floor-div/mod (level 10).
    {
        auto mul_op = op_tok_char('*') | op_tok_char('/') |
                      op_tok_id(TokenID::TK_IDIV) | op_tok_char('%');
        bin_left("mul_exp", "unop_exp", mul_op, [](const Token& o) -> BinOpKind {
            if (o.id == '*') return BinOpKind::Mul;
            if (o.id == '/') return BinOpKind::Div;
            if (o.id == static_cast<IDType>(TokenID::TK_IDIV)) return BinOpKind::FloorDiv;
            return BinOpKind::Mod;
        });
    }
    // add/sub (level 9).
    bin_left("add_exp", "mul_exp", op_tok_char('+') | op_tok_char('-'),
             [](const Token& o) -> BinOpKind { return o.id == '+' ? BinOpKind::Add : BinOpKind::Sub; });
    // concat (level 8, RIGHT-assoc): add ('..' concat)?.
    {
        auto h = (g["concat_exp"] = g["add_exp"] >> -(op_tok_id(TokenID::TK_CONCAT) >> g["concat_exp"]));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode lhs,
                                    std::optional<std::tuple<Token, AstNode>> rhs) -> AstNode {
            if (!rhs) return lhs;
            BinOp b;
            b.op = BinOpKind::Concat;
            b.lhs = Box{std::move(lhs)};
            b.rhs = Box{std::move(std::get<1>(*rhs))};
            b.start = bstart(c, sp);
            b.end = bend(c, sp);
            return b;
        });
    }
    // shifts (level 7).
    bin_left("shift_exp", "concat_exp",
             op_tok_id(TokenID::TK_SHL) | op_tok_id(TokenID::TK_SHR),
             [](const Token& o) -> BinOpKind {
                 return o.id == static_cast<IDType>(TokenID::TK_SHL) ? BinOpKind::Shl : BinOpKind::Shr;
             });
    // band(6) / bxor(5) / bor(4).
    bin_left("band_exp", "shift_exp", op_tok_char('&'),
             [](const Token&) -> BinOpKind { return BinOpKind::BAnd; });
    bin_left("bxor_exp", "band_exp", op_tok_char('~'),
             [](const Token&) -> BinOpKind { return BinOpKind::BXor; });
    bin_left("bor_exp", "bxor_exp", op_tok_char('|'),
             [](const Token&) -> BinOpKind { return BinOpKind::BOr; });
    // relational (level 3).
    {
        auto rel_op = op_tok_char('<') | op_tok_char('>') |
                      op_tok_id(TokenID::TK_LE) | op_tok_id(TokenID::TK_GE) |
                      op_tok_id(TokenID::TK_NE) | op_tok_id(TokenID::TK_EQ);
        bin_left("rel_exp", "bor_exp", rel_op, [](const Token& o) -> BinOpKind {
            if (o.id == '<') return BinOpKind::Lt;
            if (o.id == '>') return BinOpKind::Gt;
            if (o.id == static_cast<IDType>(TokenID::TK_LE)) return BinOpKind::Le;
            if (o.id == static_cast<IDType>(TokenID::TK_GE)) return BinOpKind::Ge;
            if (o.id == static_cast<IDType>(TokenID::TK_NE)) return BinOpKind::Ne;
            return BinOpKind::Eq;
        });
    }
    // and (level 2) / or (level 1).
    bin_left("and_exp", "rel_exp", op_tok_id(TokenID::TK_AND),
             [](const Token&) -> BinOpKind { return BinOpKind::And; });
    bin_left("or_exp", "and_exp", op_tok_id(TokenID::TK_OR),
             [](const Token&) -> BinOpKind { return BinOpKind::Or; });

    // exp is the top of the ladder (alias; the fold flows or_exp's value up).
    g["exp"] = g["or_exp"];

    // =======================================================================
    // STATEMENTS (§9)
    // =======================================================================

    // block ::= {stat} [retstat]. Both children: vector<AstNode> and
    // optional<AstNode>. Skip/Nil results (empty ';' statement) are filtered.
    {
        auto h = (g["block"] = *g["stat"] >> -g["retstat"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, std::vector<AstNode> stats,
                                    std::optional<AstNode> ret) -> AstNode {
            Block b;
            for (auto& s : stats) {
                if (holds<Skip>(s) || holds<Nil>(s)) continue;
                b.stmts.emplace_back(Box{std::move(s)});
            }
            if (ret && !holds<Skip>(*ret) && !holds<Nil>(*ret))
                b.stmts.emplace_back(Box{std::move(*ret)});
            b.start = bstart(c, sp);
            b.end = bend(c, sp);
            return b;
        });
    }

    // --- simple statements ---
    {
        auto h = (g["stat_break"] = tok_id(TokenID::TK_BREAK));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            return Break{bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["stat_goto"] = tok_id(TokenID::TK_GOTO) >> g["name_token"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm) -> AstNode {
            return Goto{get<Name>(nm).name, bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["stat_label"] =
                      tok_id(TokenID::TK_DBCOLON) >> g["name_token"] >> tok_id(TokenID::TK_DBCOLON));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm) -> AstNode {
            return Label{get<Name>(nm).name, bstart(c, sp), bend(c, sp)};
        });
    }
    {
        auto h = (g["stat_do"] = tok_id(TokenID::TK_DO) >> g["block"] >> tok_id(TokenID::TK_END));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode body) -> AstNode {
            Do node;
            node.body = Box{std::move(body)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        auto h = (g["stat_while"] =
                      tok_id(TokenID::TK_WHILE) >> g["exp"] >> tok_id(TokenID::TK_DO) >>
                      g["block"] >> tok_id(TokenID::TK_END));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode cond, AstNode body) -> AstNode {
            While node;
            node.cond = Box{std::move(cond)};
            node.body = Box{std::move(body)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        auto h = (g["stat_repeat"] =
                      tok_id(TokenID::TK_REPEAT) >> g["block"] >> tok_id(TokenID::TK_UNTIL) >> g["exp"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode body, AstNode cond) -> AstNode {
            Repeat node;
            node.body = Box{std::move(body)};
            node.cond = Box{std::move(cond)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // if exp then block {elseif exp then block} [else block] end.
        // Keywords void → (cond, body, vector<(cond,body)>, optional<body>).
        auto h = (g["stat_if"] =
                      (tok_id(TokenID::TK_IF) >> g["exp"] >> tok_id(TokenID::TK_THEN) >> g["block"]) >>
                      *(tok_id(TokenID::TK_ELSEIF) >> g["exp"] >> tok_id(TokenID::TK_THEN) >> g["block"]) >>
                      -(tok_id(TokenID::TK_ELSE) >> g["block"]) >>
                      tok_id(TokenID::TK_END));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode cond0, AstNode body0,
                                    std::vector<std::tuple<AstNode, AstNode>> elseifs,
                                    std::optional<AstNode> else_block) -> AstNode {
            If node;
            node.branches.emplace_back(Box{std::move(cond0)}, Box{std::move(body0)});
            for (auto& [cond, body] : elseifs)
                node.branches.emplace_back(Box{std::move(cond)}, Box{std::move(body)});
            if (else_block) node.else_body = Box{std::move(*else_block)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // numeric for: for Name '=' exp ',' exp [',' exp] do block end.
        auto h = (g["stat_numfor"] =
                      tok_id(TokenID::TK_FOR) >> g["name_token"] >> tok_char('=') >> g["exp"] >>
                      tok_char(',') >> g["exp"] >> -(tok_char(',') >> g["exp"]) >>
                      tok_id(TokenID::TK_DO) >> g["block"] >> tok_id(TokenID::TK_END));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode varnm, AstNode init,
                                    AstNode limit, std::optional<AstNode> step,
                                    AstNode body) -> AstNode {
            NumericFor node;
            node.var = get<Name>(varnm).name;
            node.init = Box{std::move(init)};
            node.limit = Box{std::move(limit)};
            if (step) node.step = Box{std::move(*step)};
            node.body = Box{std::move(body)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // generic for: for namelist in explist do block end.
        auto h = (g["stat_genfor"] =
                      tok_id(TokenID::TK_FOR) >> g["namelist"] >> tok_id(TokenID::TK_IN) >>
                      g["explist"] >> tok_id(TokenID::TK_DO) >> g["block"] >> tok_id(TokenID::TK_END));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode namesnode, AstNode exprsnode,
                                    AstNode body) -> AstNode {
            GenericFor node;
            for (auto& box : get<Block>(namesnode).stmts)
                node.names.push_back(get<Name>(*box).name);
            for (auto& box : get<Block>(exprsnode).stmts)
                node.exprs.emplace_back(std::move(box));
            node.body = Box{std::move(body)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }

    // attrib / attnamelist (§9). attrib ::= ['<' Name '>'] — overall optional.
    {
        auto h = (g["attrib"] = tok_char('<') >> g["name_token"] >> tok_char('>'));
        // Carry the attribute text as a Name so attnamelist can map it.
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm) -> AstNode {
            return Name{get<Name>(nm).name, bstart(c, sp), bend(c, sp)};
        });
    }
    {
        // attnamelist ::= Name attrib {',' Name attrib}. attrib is a Rule (its
        // own result type AstNode) and is optional, so each repetition
        // iteration folds to (Name, optional<AstNode>); the first Name+attrib
        // is (AstNode, optional<AstNode>). Packed into a LocalDecl carrier.
        auto h = (g["attnamelist"] =
                      g["name_token"] >> -g["attrib"] >>
                      *(tok_char(',') >> g["name_token"] >> -g["attrib"]));
        h.set_action(
            [](Ctx&, Span, AstNode first_nm, std::optional<AstNode> first_attr,
               std::vector<std::tuple<AstNode, std::optional<AstNode>>> rest) -> AstNode {
                LocalDecl carrier;
                carrier.names.push_back(get<Name>(first_nm).name);
                carrier.attribs.push_back(first_attr ? attrib_of(get<Name>(*first_attr).name)
                                                     : Attrib::None);
                for (auto& [nm, attr] : rest) {
                    carrier.names.push_back(get<Name>(nm).name);
                    carrier.attribs.push_back(attr ? attrib_of(get<Name>(*attr).name) : Attrib::None);
                }
                return carrier;
            });
    }
    {
        // local attnamelist ['=' explist]. '=' void → optional<AstNode>.
        auto h = (g["stat_local"] =
                      tok_id(TokenID::TK_LOCAL) >> g["attnamelist"] >> -(tok_char('=') >> g["explist"]));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode carrier,
                                    std::optional<AstNode> values) -> AstNode {
            LocalDecl node = std::move(get<LocalDecl>(carrier));
            if (values) {
                std::vector<Box> vals;
                for (auto& box : get<Block>(*values).stmts) vals.emplace_back(std::move(box));
                node.values = std::move(vals);
            }
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // local function Name funcbody.
        auto h = (g["stat_localfunc"] =
                      tok_id(TokenID::TK_LOCAL) >> tok_id(TokenID::TK_FUNCTION) >> g["name_token"] >> g["funcbody"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm, AstNode bodynode) -> AstNode {
            LocalFunction node;
            node.name = get<Name>(nm).name;
            node.body = std::move(get<FuncBody>(bodynode));
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }

    // function funcname funcbody (§9). funcname ::= Name {'.' Name} [':' Name].
    {
        auto h = (g["funcname"] =
                      g["name_token"] >> *(tok_char('.') >> g["name_token"]) >>
                      -(tok_char(':') >> g["name_token"]));
        h.set_action([](Ctx&, Span, AstNode base, std::vector<AstNode> dotted,
                        std::optional<AstNode> method) -> AstNode {
            FuncName fn;
            fn.fields.push_back(get<Name>(base).name);
            for (auto& nm : dotted) fn.fields.push_back(get<Name>(nm).name);
            if (method) fn.method = get<Name>(*method).name;
            return fn;
        });
    }
    {
        auto h = (g["stat_function"] = tok_id(TokenID::TK_FUNCTION) >> g["funcname"] >> g["funcbody"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode fnname, AstNode bodynode) -> AstNode {
            FuncStat node;
            node.name = std::move(get<FuncName>(fnname));
            node.body = std::move(get<FuncBody>(bodynode));
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }

    // --- assignment vs function-call statement (§9 stat ambiguity) ---
    //   stat ::= varlist '=' explist | functioncall
    // Multi-target assignment needs varlist = var {',' var}; single-target is
    // var '=' explist. A bare var/functioncall with no '=' is a call statement.
    {
        // varlist '=' explist (≥2 targets).
        auto h = (g["stat_assign"] =
                      g["var"] >> +(tok_char(',') >> g["var"]) >> tok_char('=') >> g["explist"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode first, std::vector<AstNode> more,
                                    AstNode values) -> AstNode {
            Assign a;
            a.targets.emplace_back(Box{std::move(first)});
            for (auto& v : more) a.targets.emplace_back(Box{std::move(v)});
            for (auto& box : get<Block>(values).stmts) a.values.emplace_back(std::move(box));
            a.start = bstart(c, sp);
            a.end = bend(c, sp);
            return a;
        });
    }
    {
        // Single-target assignment: var '=' explist.
        auto h = (g["stat_assign1"] = g["var"] >> tok_char('=') >> g["explist"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode target, AstNode values) -> AstNode {
            Assign a;
            a.targets.emplace_back(Box{std::move(target)});
            for (auto& box : get<Block>(values).stmts) a.values.emplace_back(std::move(box));
            a.start = bstart(c, sp);
            a.end = bend(c, sp);
            return a;
        });
    }
    {
        // functioncall as a statement.
        auto h = (g["stat_call"] = g["functioncall"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode call) -> AstNode {
            CallStat cs;
            cs.call = Box{std::move(call)};
            cs.start = bstart(c, sp);
            cs.end = bend(c, sp);
            return cs;
        });
    }

    // retstat ::= return [explist] [';']. ';' void → no trailing param.
    {
        auto h = (g["retstat"] = tok_id(TokenID::TK_RETURN) >> -g["explist"] >> -tok_char(';'));
        h.set_action([bstart, bend](Ctx& c, Span sp, std::optional<AstNode> values) -> AstNode {
            Return r;
            if (values) {
                std::vector<Box> vals;
                for (auto& box : get<Block>(*values).stmts) vals.emplace_back(std::move(box));
                r.values = std::move(vals);
            }
            r.start = bstart(c, sp);
            r.end = bend(c, sp);
            return r;
        });
    }
    {
        // ';' empty statement → Skip (filtered by block).
        auto h = (g["stat_empty"] = tok_char(';'));
        h.set_action([bstart, bend](Ctx& c, Span sp) -> AstNode {
            return Skip{bstart(c, sp), bend(c, sp)};
        });
    }

    // stat dispatch (ordered alternation). stat_localfunc before stat_local so
    // `local function` is not misread as `local attnamelist`. stat_assign
    // (multi) before stat_assign1 (single) before stat_call so the longest
    // match commits first.
    {
        auto h = (g["stat"] =
                      g["stat_empty"] |
                      g["stat_localfunc"] |
                      g["stat_local"] |
                      g["stat_function"] |
                      g["stat_if"] |
                      g["stat_while"] |
                      g["stat_do"] |
                      g["stat_repeat"] |
                      g["stat_numfor"] |
                      g["stat_genfor"] |
                      g["stat_break"] |
                      g["stat_goto"] |
                      g["stat_label"] |
                      g["stat_assign"] |
                      g["stat_assign1"] |
                      g["stat_call"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }

    // =======================================================================
    // chunk ::= block   (§9 — verbatim. NO explicit EOS in the grammar.)
    //
    // The EOS sentinel exists in the token stream but is NOT referenced here.
    // peglib's left-recursion seed-grow converges only when a left-recursive
    // rule is reached without an intervening sequence at the start position:
    // a `chunk = block EOS` sequence wrapper pins the match end and prevents
    // the seed from growing through the var/functioncall/prefixexp triangle
    // (verified empirically — the alias wrapper `chunk = block` does NOT have
    // this problem). Since the EBNF itself is `chunk ::= block` (EOS is not a
    // grammar symbol), we keep it verbatim and check full consumption in
    // Parser::parse() instead.
    // =======================================================================
    {
        auto h = (g["chunk"] = g["block"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode bodynode) -> AstNode {
            Chunk chunk;
            chunk.body = std::move(get<Block>(bodynode));
            chunk.start = bstart(c, sp);
            chunk.end = bend(c, sp);
            return chunk;
        });
    }

    g.set_start("chunk");
    return g;
}

// Map an attribute spelling to the Attrib enum ("const"/"close").
inline Attrib attrib_of(const std::string& s)
{
    if (s == "const") return Attrib::Const;
    if (s == "close") return Attrib::Close;
    return Attrib::None;
}

} // namespace ys::lua::parserconv
