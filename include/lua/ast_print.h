#pragma once

// ASTPrinter: render an AstNode tree as an indented S-expression, for
// debugging and test assertions.
//
// Output format (one node per line, 2-space indent per level):
//
//   (Chunk
//     (Block
//       (LocalDecl (Name "x") = (IntLit 1))  ;@1:1
//       (Return (Name "x"))                  ;@2:1
//       ))                                   ;@2:8
//
// Each node carries a source-span comment `;@line:col` computed from the
// node's `start` byte offset via the (optional) SourceMap. If no SourceMap is
// supplied, the comment shows the raw byte offset instead: `;@42`.
//
// This is debug/test output — not stable, not minimal. The shape (node kind
// + key fields + children) is what tests assert on.

#include <cstddef>
#include <sstream>
#include <string>

#include "lua/ast.h"
#include "peglib.h"  // peg::SourceMap, peg::SourceLocation

namespace ys
{
    namespace lua
    {
        class ASTPrinter {
        public:
            // Render the whole tree. If map is non-null, source spans are
            // formatted as line:col; otherwise as raw byte offsets.
            static std::string to_sexp(const AstNode& root,
                                       const peg::SourceMap* map = nullptr)
            {
                std::ostringstream os;
                Printer p{os, map, 0};
                p.emit(root);
                return os.str();
            }

        private:
            ASTPrinter() = delete;  // static-only facade; use via the inner Printer.

            struct Printer {
                std::ostringstream& os;
                const peg::SourceMap* map;
                std::size_t depth;

                void indent()
                {
                    for (std::size_t i = 0; i < depth; ++i) os << "  ";
                }

                // Source-span annotation for the current node.
                void span(std::size_t start)
                {
                    os << "  ;@";
                    if (map) {
                        auto loc = map->locate(start);
                        os << loc.line << ':' << loc.column;
                    } else {
                        os << start;
                    }
                }

                // Emit a child held in a Box, recursing with +1 depth.
                void child(const Box& b)
                {
                    if (b) emit(*b);
                }

                // Main dispatch.
                void emit(const AstNode& n)
                {
                    indent();
                    std::visit([this](const auto& x) { this->one(x); }, n.v);
                }

                // --- per-kind emitters ---

                void one(const Nil&)
                {
                    os << "(nil)";
                    // no span on the placeholder; tests don't depend on it
                }
                void one(const Skip&)   { os << "(skip)"; }
                void one(const True&)   { os << "(True)"; }
                void one(const False&)  { os << "(False)"; }
                void one(const IntLit& x)
                {
                    os << "(IntLit " << x.value << ")";
                    span(x.start);
                    os << '\n';
                }
                void one(const FltLit& x)
                {
                    os << "(FltLit " << x.value << ")";
                    span(x.start);
                    os << '\n';
                }
                void one(const StrLit& x)
                {
                    os << "(StrLit \"" << x.value << "\")";
                    span(x.start);
                    os << '\n';
                }
                void one(const Vararg&)
                {
                    os << "(Vararg ...)";
                }
                void one(const Name& x)
                {
                    os << "(Name \"" << x.name << "\")";
                    span(x.start);
                    os << '\n';
                }

                // Helper: a BinOpKind name for printing.
                static const char* binop_name(BinOpKind k)
                {
                    switch (k) {
                    case BinOpKind::Add: return "+";
                    case BinOpKind::Sub: return "-";
                    case BinOpKind::Mul: return "*";
                    case BinOpKind::Div: return "/";
                    case BinOpKind::FloorDiv: return "//";
                    case BinOpKind::Mod: return "%";
                    case BinOpKind::Pow: return "^";
                    case BinOpKind::Concat: return "..";
                    case BinOpKind::Lt: return "<";
                    case BinOpKind::Le: return "<=";
                    case BinOpKind::Gt: return ">";
                    case BinOpKind::Ge: return ">=";
                    case BinOpKind::Eq: return "==";
                    case BinOpKind::Ne: return "~=";
                    case BinOpKind::BAnd: return "&";
                    case BinOpKind::BOr: return "|";
                    case BinOpKind::BXor: return "~";
                    case BinOpKind::Shl: return "<<";
                    case BinOpKind::Shr: return ">>";
                    case BinOpKind::And: return "and";
                    case BinOpKind::Or: return "or";
                    }
                    return "?";
                }
                static const char* unop_name(UnOpKind k)
                {
                    switch (k) {
                    case UnOpKind::Neg: return "-";
                    case UnOpKind::Not: return "not";
                    case UnOpKind::Len: return "#";
                    case UnOpKind::BNot: return "~";
                    }
                    return "?";
                }
                static const char* attrib_name(Attrib a)
                {
                    switch (a) {
                    case Attrib::None: return "";
                    case Attrib::Const: return "<const>";
                    case Attrib::Close: return "<close>";
                    }
                    return "?";
                }

                // Two-child nodes with a printed operator.
                void one(const BinOp& x)
                {
                    os << "(BinOp " << binop_name(x.op) << '\n';
                    ++depth; child(x.lhs); child(x.rhs); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const UnOp& x)
                {
                    os << "(UnOp " << unop_name(x.op) << '\n';
                    ++depth; child(x.operand); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Paren& x)
                {
                    os << "(Paren\n";
                    ++depth; child(x.exp); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Index& x)
                {
                    os << "(Index\n";
                    ++depth; child(x.obj); child(x.key); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Field& x)
                {
                    os << "(Field ." << x.name << '\n';
                    ++depth; child(x.obj); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Call& x)
                {
                    os << "(Call\n";
                    ++depth; child(x.func);
                    for (const auto& a : x.args) child(a);
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const MethodCall& x)
                {
                    os << "(MethodCall :" << x.method << '\n';
                    ++depth; child(x.obj);
                    for (const auto& a : x.args) child(a);
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }

                // Helper: print a FieldEntry. The value is a sub-tree emitted
                // at +1 depth so multi-line values stay readable.
                void field(const FieldEntry& f)
                {
                    indent();
                    switch (f.kind) {
                    case FieldEntry::Kind::Positional:
                        os << "(field-pos\n";
                        ++depth; child(f.value); --depth;
                        break;
                    case FieldEntry::Kind::Bracketed:
                        os << "(field-bracket\n";
                        ++depth;
                        indent(); os << "(key\n"; ++depth; child(f.key); --depth;
                        indent(); os << ")\n";
                        child(f.value);
                        --depth;
                        break;
                    case FieldEntry::Kind::Named:
                        os << "(field-named " << f.name << "\n";
                        ++depth; child(f.value); --depth;
                        break;
                    }
                    indent(); os << ")\n";
                }

                void one(const TableCtor& x)
                {
                    os << "(TableCtor\n";
                    ++depth;
                    for (const auto& f : x.fields) field(f);
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                // FieldEntry appears standalone only as an intermediate carrier
                // in table field parsing; print it the same way a TableCtor
                // field prints.
                void one(const FieldEntry& x) { field(x); }
                // FuncName: dotted path with optional :method. A bare carrier
                // node (no span yet); print without a span annotation.
                void one(const FuncName& x)
                {
                    os << "(FuncName";
                    bool first = true;
                    for (const auto& f : x.fields) {
                        if (!first) os << '.';
                        first = false;
                        os << f;
                    }
                    if (x.method) os << ':' << *x.method;
                    os << ")\n";
                }

                // FuncBody/FuncDef: print params then body.
                void funcbody(const FuncBody& fb, const char* head)
                {
                    os << head << " (";
                    bool first = true;
                    for (const auto& p : fb.params) {
                        if (!first) os << ' ';
                        first = false;
                        if (p.kind == Param::Kind::Vararg) os << "...";
                        else os << p.name;
                    }
                    os << ")\n";
                    ++depth; child(fb.body); --depth;
                }
                void one(const FuncBody& x)
                {
                    funcbody(x, "(FuncBody");
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const FuncDef& x)
                {
                    // function funcbody: FuncDef wraps a FuncBody child
                    // (FuncBody owns the params + block). Emitting the nested
                    // FuncBody — rather than flattening params under FuncDef —
                    // keeps the printer faithful to the AST shape and lets
                    // callers match "(FuncBody (params...)".
                    os << "(FuncDef\n";
                    ++depth;
                    one(x.body);  // by-value FuncBody; mirrors Chunk→Block
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }

                // Block: (Block <stmts...>).
                void one(const Block& x)
                {
                    os << "(Block\n";
                    ++depth;
                    for (const auto& s : x.stmts) child(s);
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Chunk& x)
                {
                    os << "(Chunk\n";
                    ++depth;
                    // Chunk holds a Block by value (not a Box); print it
                    // directly without wrapping in a temporary AstNode (which
                    // would copy the move-only Block).
                    one(x.body);
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }

                // --- statements ---
                void one(const Assign& x)
                {
                    os << "(Assign\n";
                    ++depth;
                    indent(); os << "(targets\n";
                    ++depth; for (const auto& t : x.targets) child(t); --depth;
                    indent(); os << ")\n";
                    indent(); os << "(values\n";
                    ++depth; for (const auto& v : x.values) child(v); --depth;
                    indent(); os << ")\n";
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const CallStat& x)
                {
                    os << "(CallStat\n";
                    ++depth; child(x.call); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Do& x)
                {
                    os << "(Do\n";
                    ++depth; child(x.body); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const While& x)
                {
                    os << "(While\n";
                    ++depth; child(x.cond); child(x.body); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Repeat& x)
                {
                    os << "(Repeat\n";
                    ++depth; child(x.body); child(x.cond); --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const If& x)
                {
                    os << "(If\n";
                    ++depth;
                    for (const auto& [cond, body] : x.branches) {
                        indent(); os << "(then\n";
                        ++depth; child(cond); child(body); --depth;
                        indent(); os << ")\n";
                    }
                    if (x.else_body) {
                        indent(); os << "(else\n";
                        ++depth; child(*x.else_body); --depth;
                        indent(); os << ")\n";
                    }
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const NumericFor& x)
                {
                    os << "(NumericFor " << x.var << '\n';
                    ++depth;
                    indent(); os << "(init "; child(x.init); os << ")\n";
                    indent(); os << "(limit "; child(x.limit); os << ")\n";
                    if (x.step) { indent(); os << "(step "; child(*x.step); os << ")\n"; }
                    child(x.body);
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const GenericFor& x)
                {
                    os << "(GenericFor (";
                    bool first = true;
                    for (const auto& nm : x.names) {
                        if (!first) os << ' '; first = false;
                        os << nm;
                    }
                    os << ") in\n";
                    ++depth;
                    for (const auto& e : x.exprs) child(e);
                    child(x.body);
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const LocalDecl& x)
                {
                    os << "(LocalDecl";
                    for (std::size_t i = 0; i < x.names.size(); ++i) {
                        os << ' ' << x.names[i];
                        if (x.attribs.size() > i && x.attribs[i] != Attrib::None)
                            os << attrib_name(x.attribs[i]);
                    }
                    if (x.values) {
                        os << " =\n";
                        ++depth;
                        for (const auto& v : *x.values) child(v);
                        --depth;
                        indent();
                        os << ')';
                    } else {
                        // No values: close on the same line so a declaration-
                        // only local renders as a compact leaf,
                        // e.g. (LocalDecl y<close>).
                        os << ')';
                    }
                    span(x.start); os << '\n';
                }
                void one(const LocalFunction& x)
                {
                    os << "(LocalFunction " << x.name << '\n';
                    ++depth;
                    // x.body is a FuncBody by value; print directly (it is
                    // move-only, so cannot be wrapped in a temp AstNode).
                    funcbody(x.body, "(body");
                    indent(); os << ")\n";
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const FuncStat& x)
                {
                    os << "(FuncStat ";
                    bool first = true;
                    for (const auto& f : x.name.fields) {
                        if (!first) os << '.';
                        first = false;
                        os << f;
                    }
                    if (x.name.method) os << ':' << *x.name.method;
                    os << '\n';
                    ++depth;
                    funcbody(x.body, "(body");
                    indent(); os << ")\n";
                    --depth;
                    indent(); os << ')';
                    span(x.start); os << '\n';
                }
                void one(const Goto& x)
                {
                    os << "(Goto " << x.label << ")";
                    span(x.start); os << '\n';
                }
                void one(const Label& x)
                {
                    os << "(Label " << x.name << ")";
                    span(x.start); os << '\n';
                }
                void one(const Break& x)
                {
                    os << "(Break)";
                    span(x.start); os << '\n';
                }
                void one(const Return& x)
                {
                    if (x.values) {
                        os << "(Return\n";
                        ++depth;
                        for (const auto& v : *x.values) child(v);
                        --depth;
                        indent(); os << ')';
                    } else {
                        os << "(Return)";
                    }
                    span(x.start); os << '\n';
                }
            };
        };

    } // namespace lua
} // namespace ys
