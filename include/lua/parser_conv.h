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
    // prefixexp / var / functioncall — a SUFFIX LOOP (NOT the §9 left
    // recursion). This is the load-bearing deviation from the Lua 5.4 EBNF,
    // and it is mandatory: the verbatim §9 triangle is NOT executable as PEG.
    //
    //   §9 (specification, left-recursive, unparseable in PEG):
    //     prefixexp    ::= var | functioncall | '(' exp ')'
    //     var          ::= Name | prefixexp '[' exp ']' | prefixexp '.' Name
    //     functioncall ::= prefixexp args | prefixexp ':' Name args
    //
    //   PEG here (executable, same language):
    //     primary       ::= Name | '(' exp ')'
    //     suffix        ::= '.' Name | '[' exp ']' | args | ':' Name args
    //     prefixexp     ::= primary { suffix }
    //
    // WHY the verbatim form fails in PEG (not a peglib bug): `var` and
    // `functioncall` share the `prefixexp` prefix, and PEG's ordered choice
    // commits to the first alternative that succeeds *at that prefix*, before
    // the distinguishing suffix (`args`) is even visible. So no single branch
    // order can satisfy both call sites: the EXPRESSION site (simple_exp →
    // prefixexp) needs prefixexp to absorb a trailing call, while the CALL
    // site (call_plain = prefixexp args) needs it to stop before one. This is
    // the same reason the reference C parser uses a suffix loop (`lcode.c`
    // `suffixedexp`): the §9 EBNF is a specification, not an executable
    // procedure, and every deterministic parser (PEG, recursive descent) must
    // express it as a suffix loop. peglib's seed-grow left recursion works on
    // cooperating shapes (see its lr_token_triangle_test) but cannot resolve
    // this shared-prefix mutual recursion — confirmed empirically: every
    // input parses under SOME alternation order, but no single order parses
    // all inputs.
    //
    // The suffix loop below replaces the entire var/functioncall/prefixexp
    // triangle. Each suffix is its own rule (so the inner alternation has a
    // uniform AstNode result type) and builds its node with the base slot
    // (Field.obj / Index.obj / Call.func / MethodCall.obj) LEFT EMPTY; the
    // prefixexp fold fills that slot from the running accumulator, producing
    // left-associative nesting (a.b.c → Field(c, Field(b, Name(a)))).
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

    // -----------------------------------------------------------------------
    // SUFFIX-LOOP grammar for prefixexp/var/functioncall (see the design
    // comment above). Each suffix rule builds its node with the base slot
    // empty; the prefixexp fold fills it from the accumulator. Defined before
    // `args` uses them? No — args is already defined above; these reference
    // g["args"], g["name_token"], g["exp"], all of which precede this block.
    // -----------------------------------------------------------------------

    // Base case (§9 primaryexp): a Name, or '(' exp ')' → Paren.
    {
        // '(' exp ')' → Paren{exp}. Preserved in the AST (not collapsed) so a
        // later multires-adjustment pass can see parens (§3.4.12: (f()) vs f()).
        auto h = (g["primary_parens"] = tok_char('(') >> g["exp"] >> tok_char(')'));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode e) -> AstNode {
            Paren node;
            node.exp = Box{std::move(e)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // primary ::= Name | '(' exp ')'. name_token already yields Name.
        auto h = (g["primary"] = g["name_token"] | g["primary_parens"]);
        h.set_action([](Ctx&, Span, AstNode n) -> AstNode { return n; });
    }

    // --- the four suffix forms (each leaves the base slot empty) ---
    {
        // '.' Name → Field{obj=∅, name}. obj filled by the prefixexp fold.
        auto h = (g["suffix_field"] = tok_char('.') >> g["name_token"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm) -> AstNode {
            Field node;
            node.name = get<Name>(nm).name;  // no leading dot; printer adds it
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // '[' exp ']' → Index{obj=∅, key}. obj filled by the fold.
        auto h = (g["suffix_index"] = tok_char('[') >> g["exp"] >> tok_char(']'));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode key) -> AstNode {
            Index node;
            node.key = Box{std::move(key)};
            node.start = bstart(c, sp);
            node.end = bend(c, sp);
            return node;
        });
    }
    {
        // ':' Name args → MethodCall{obj=∅, method, args}. obj filled by the
        // fold. args unwraps exactly like the old call_method/call_method_tail
        // (args_paren carries the arg list; table/string is a single arg).
        auto h = (g["suffix_method"] = tok_char(':') >> g["name_token"] >> g["args"]);
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode nm, AstNode argsnode)
                         -> AstNode {
            MethodCall node;
            node.method = get<Name>(nm).name;  // no colon; printer adds it
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
    // suffix_call reuses the existing g["args"] rule directly (it yields
    // Call [from args_paren, func=∅] | TableCtor | StrLit). The prefixexp fold
    // discriminates these, so no separate rule is needed: the suffix-loop
    // alternation is suffix_field | suffix_index | args | suffix_method.

    // The suffix loop itself: primary { suffix }.
    //
    // The alternation of named rules collapses to a uniform AstNode, so the
    // repetition folds to std::vector<AstNode> (one element per suffix). The
    // action is a left-fold that wraps the accumulator into each suffix node,
    // producing left-associative nesting:
    //   a.b.c    → Field(c, Field(b, Name(a)))
    //   t[1][2]  → Index([2], Index([1], Name(t)))
    //   f()()    → Call(Call(Name(f)))
    //   o:m(42)  → MethodCall(Name(o), :m, [42])
    {
        auto h = (g["prefixexp"] =
                      g["primary"] >>
                      *(g["suffix_field"] | g["suffix_index"] | g["args"] | g["suffix_method"]));
        h.set_action([bstart, bend](Ctx& c, Span sp, AstNode first,
                                     std::vector<AstNode> rest) -> AstNode {
            AstNode acc = std::move(first);
            for (auto& r : rest) {
                if (holds<Field>(r)) {
                    get<Field>(r).obj = Box{std::move(acc)};
                    acc = std::move(r);
                } else if (holds<Index>(r)) {
                    get<Index>(r).obj = Box{std::move(acc)};
                    acc = std::move(r);
                } else if (holds<MethodCall>(r)) {
                    get<MethodCall>(r).obj = Box{std::move(acc)};
                    acc = std::move(r);
                } else if (holds<Call>(r)) {
                    // args_paren → existing Call (func=∅), fill func, keep args.
                    get<Call>(r).func = Box{std::move(acc)};
                    acc = std::move(r);
                } else {
                    // tableconstructor or literal_string (from g["args"]) → the
                    // whole value becomes the single arg of a fresh Call whose
                    // func is the accumulator. Span taken from the loop, mirroring
                    // the prior functioncall wrap branch.
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
            // §3.4.11 sugar: `function a.b:c (...) body end` is equivalent to
            // `a.b.c = function(self, ...) body end`. When a method name is
            // present, prepend an implicit `self` parameter (the receiver) so
            // the body can name it. This is the desugar reference Lua performs.
            if (node.name.method) {
                Param self;
                self.kind = Param::Kind::Name;
                self.name = "self";
                node.body.params.insert(node.body.params.begin(),
                                        std::move(self));
            }
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
        // varlist '=' explist (≥2 targets). Each target is a prefixexp that
        // must be an lvalue (var form); the grammar accepts any suffixexp and
        // lvalue-ness is enforced later (matches reference Lua's structure).
        auto h = (g["stat_assign"] =
                      g["prefixexp"] >> +(tok_char(',') >> g["prefixexp"]) >> tok_char('=') >> g["explist"]);
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
        // Single-target assignment: prefixexp '=' explist.
        auto h = (g["stat_assign1"] = g["prefixexp"] >> tok_char('=') >> g["explist"]);
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
        // A function call used as a statement (§9: stat ::= functioncall).
        // Reaches here only when the prefixexp had no trailing '=' (the
        // assignment rules above are tried first in the stat alternation and
        // consume any '=' themselves). The full suffixexp is parsed; a real
        // Lua compiler would reject a non-call result here (e.g. a bare `a.b`
        // statement), but that lvalue-vs-call validation is deferred to a
        // later pass — the parse succeeds and the node is wrapped in CallStat.
        auto h = (g["stat_call"] = g["prefixexp"]);
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
    // The EOS sentinel exists in the token stream but is NOT referenced here:
    // a `chunk = block EOS` sequence would pin the match end, and since the
    // grammar now has NO left-recursive rules at all (the prefixexp triangle
    // was replaced by a suffix loop), the only reason to avoid the EOS pin is
    // cleanliness. The EBNF itself is `chunk ::= block` (EOS is not a grammar
    // symbol), so we keep it verbatim and check full consumption in
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
