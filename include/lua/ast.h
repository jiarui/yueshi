#pragma once

// Lua 5.4 AST node types.
//
// AstNode is a single std::variant and is the NodeType of the parser's
// Grammar<Token, AstNode>: semantic actions return an AstNode by value. Nil
// is the first alternative so the variant is default-constructible.
//
// Every node carries a source byte range [start, end) (matching Token) so
// diagnostics and ASTPrinter can point at any node.
//
// Because child nodes are held through Box (a unique_ptr wrapper), AstNode is
// move-only — copies would duplicate ownership. Actions move values out of
// the parse tree; peglib does not reuse a node's value after its action runs.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ys
{
    namespace lua
    {
        // Box<AstNode>: a recursive wrapper. std::variant cannot contain
        // itself by value (it is incomplete while its alternatives are being
        // defined), so child nodes are held through Box — a unique_ptr with
        // value semantics. Leaf nodes (literals, names) allocate zero Boxes;
        // only interior nodes allocate one Box per child edge.
        struct AstNode;  // defined below; Box references it.
        struct Nil;      // first alternative

        using Node = AstNode;

        struct Box {
            std::unique_ptr<AstNode> p;
            Box() = default;
            Box(std::unique_ptr<AstNode> x) : p(std::move(x)) {}
            Box(AstNode x);  // defined after AstNode
            Box(const Box&) = delete;
            Box& operator=(const Box&) = delete;
            Box(Box&&) noexcept = default;
            Box& operator=(Box&&) noexcept = default;
            AstNode& operator*() { return *p; }
            const AstNode& operator*() const { return *p; }
            AstNode* operator->() { return p.get(); }
            const AstNode* operator->() const { return p.get(); }
            explicit operator bool() const noexcept { return static_cast<bool>(p); }
        };
        enum class BinOpKind : unsigned char {
            // arithmetic
            Add, Sub, Mul, Div, FloorDiv, Mod, Pow,    // + - * / // % ^
            Concat,                                     // ..
            // relational
            Lt, Le, Gt, Ge, Eq, Ne,                     // < <= > >= == ~=
            // bitwise
            BAnd, BOr, BXor, Shl, Shr,                  // & | ~ << >>
            // logical
            And, Or,                                    // and or
        };

        enum class UnOpKind : unsigned char {
            Neg, Not, Len, BNot,                        // - not # ~
        };

        // Variable attribute: <const>, <close>, or none (Lua 5.4 §3.3.7).
        enum class Attrib : unsigned char { None, Const, Close };

        // -------------------------------------------------------------------
        // Node definitions. Each is an aggregate; start/end are the source
        // span. A default-constructed node has start==end==0.
        //
        // Convention: members that are sub-expressions use `Box` (a
        // value-semantics unique_ptr<AstNode>, see above); lists use
        // std::vector<Box>; names use std::string. Box is necessary because a
        // std::variant cannot contain itself by value.
        // -------------------------------------------------------------------

        // First alternative — must be default-constructible (peglib requires
        // NodeType default-constructible, and std::variant default-constructs
        // the first alternative). Represents both Lua `nil` and "no value"
        // (e.g. a transparent rule result that parents skip).
        struct Nil { std::size_t start{0}, end{0}; };

        // Transparent result for rules whose match should not appear in the
        // AST (operator/keyword tokens consumed for structure only). Parents
        // skip children whose value holds Skip.
        struct Skip { std::size_t start{0}, end{0}; };

        // --- Literals ---
        struct True   { std::size_t start{0}, end{0}; };
        struct False  { std::size_t start{0}, end{0}; };
        struct IntLit { long long value{0}; std::size_t start{0}, end{0}; };
        struct FltLit { double value{0.0}; std::size_t start{0}, end{0}; };
        struct StrLit { std::string value; std::size_t start{0}, end{0}; };
        struct Vararg { std::size_t start{0}, end{0}; };

        // --- Primary expressions ---
        // A bare identifier reference. (§9: var ::= Name | ...)
        struct Name { std::string name; std::size_t start{0}, end{0}; };

        // Binary operator. (§9: exp binop exp)
        struct BinOp {
            BinOpKind op{};
            Box lhs{};
            Box rhs{};
            std::size_t start{0}, end{0};
        };

        // Unary prefix operator. (§9: unop exp)
        struct UnOp {
            UnOpKind op{};
            Box operand{};
            std::size_t start{0}, end{0};
        };

        // A parenthesized expression. PRESERVED in the AST (not collapsed)
        // because (f()) truncates multires while f() does not (§3.4.12); the
        // later adjustment pass needs to see the parens.
        struct Paren { Box exp{}; std::size_t start{0}, end{0}; };

        // Index access: prefixexp '[' exp ']'. (§9: var ::= prefixexp '[' exp ']')
        struct Index { Box obj{}; Box key{}; std::size_t start{0}, end{0}; };

        // Field access: prefixexp '.' Name. (§9: var ::= prefixexp '.' Name)
        struct Field { Box obj{}; std::string name; std::size_t start{0}, end{0}; };

        // Function call: prefixexp args. (§9: functioncall)
        struct Call { Box func{}; std::vector<Box> args; std::size_t start{0}, end{0}; };

        // Method call: prefixexp ':' Name args. (§9: functioncall)
        struct MethodCall {
            Box obj{}; std::string method; std::vector<Box> args;
            std::size_t start{0}, end{0};
        };

        // Table constructor field (§9: field). Three forms:
        //   - Positional: just an exp (keys 1, 2, 3 ... assigned later).
        //   - [exp] = exp: computed key.
        //   - Name = exp:  named key.
        struct FieldEntry {
            enum class Kind : unsigned char { Positional, Bracketed, Named };
            Kind kind{Kind::Positional};
            Box key{};        // Bracketed only; empty otherwise
            Box value{};      // always present
            std::string name;  // Named only
            std::size_t start{0}, end{0};
        };

        // Table constructor: '{' [fieldlist] '}'. (§9: tableconstructor)
        struct TableCtor {
            std::vector<FieldEntry> fields;
            std::size_t start{0}, end{0};
        };

        // Function parameter: a Name or Vararg. (§9: parlist)
        struct Param {
            enum class Kind : unsigned char { Name, Vararg };
            Kind kind{Kind::Name};
            std::string name;  // Name only
            std::size_t start{0}, end{0};
        };

        // Function body: '(' [parlist] ')' block end. (§9: funcbody)
        // A function definition is `function funcbody` (§9: functiondef); we
        // wrap the body so FuncDef/FuncStat/LocalFunction can all share it.
        struct Block;  // forward (used in FuncBody)
        struct FuncBody {
            std::vector<Param> params;
            Box body{};       // holds a Block
            std::size_t start{0}, end{0};
        };

        // function funcbody — the function expression. (§9: functiondef)
        struct FuncDef { FuncBody body; std::size_t start{0}, end{0}; };

        // --- Statements ---
        // A block: {stat} [retstat]. (§9: block)
        struct Block {
            std::vector<Box> stmts;
            std::size_t start{0}, end{0};
        };

        // chunk ::= block. (§9: chunk) Root of every parse.
        struct Chunk { Block body; std::size_t start{0}, end{0}; };

        // varlist '=' explist. (§9: stat)
        struct Assign {
            std::vector<Box> targets;  // lvalues (var forms)
            std::vector<Box> values;
            std::size_t start{0}, end{0};
        };

        // A function call used as a statement. (§9: stat ::= functioncall)
        struct CallStat { Box call{}; std::size_t start{0}, end{0}; };

        // do block end. (§9: stat)
        struct Do { Box body{}; std::size_t start{0}, end{0}; };

        // while exp do block end. (§9: stat)
        struct While { Box cond{}; Box body{}; std::size_t start{0}, end{0}; };

        // repeat block until exp. (§9: stat)
        struct Repeat { Box body{}; Box cond{}; std::size_t start{0}, end{0}; };

        // if exp then block {elseif exp then block} [else block] end. (§9: stat)
        // branches: (condition, body) pairs; else_body is the optional else.
        struct If {
            std::vector<std::pair<Box, Box>> branches;
            std::optional<Box> else_body;
            std::size_t start{0}, end{0};
        };

        // for Name '=' exp ',' exp [',' exp] do block end. (§9: stat)
        struct NumericFor {
            std::string var;
            Box init{};
            Box limit{};
            std::optional<Box> step;
            Box body{};
            std::size_t start{0}, end{0};
        };

        // for namelist in explist do block end. (§9: stat)
        struct GenericFor {
            std::vector<std::string> names;
            std::vector<Box> exprs;
            Box body{};
            std::size_t start{0}, end{0};
        };

        // local attnamelist ['=' explist]. (§9: stat)
        struct LocalDecl {
            std::vector<std::string> names;
            std::vector<Attrib> attribs;
            std::optional<std::vector<Box>> values;
            std::size_t start{0}, end{0};
        };

        // local function Name funcbody. (§9: stat)
        // NOTE: Lua desugars this to `local f; f = function() body end` (so the
        // body can reference f recursively). The parser emits LocalFunction;
        // the lowering pass performs the two-step desugar. We do NOT desugar
        // in the parser.
        struct LocalFunction {
            std::string name;
            FuncBody body;
            std::size_t start{0}, end{0};
        };

        // The funcname of a function-definition statement: Name {'.' Name} [':' Name].
        // (§9: funcname) fields are dotted path; method (optional) is the :Name.
        struct FuncName {
            std::vector<std::string> fields;  // fields[0] is the base Name
            std::optional<std::string> method;
            std::size_t start{0}, end{0};
        };

        // function funcname funcbody. (§9: stat)
        struct FuncStat {
            FuncName name;
            FuncBody body;
            std::size_t start{0}, end{0};
        };

        // goto Name. (§9: stat)
        struct Goto { std::string label; std::size_t start{0}, end{0}; };

        // '::' Name '::'. (§9: label)
        struct Label { std::string name; std::size_t start{0}, end{0}; };

        // break. (§9: stat)
        struct Break { std::size_t start{0}, end{0}; };

        // return [explist] [';']. (§9: retstat)
        struct Return {
            std::optional<std::vector<Box>> values;
            std::size_t start{0}, end{0};
        };

        // The variant. Nil is first (default-constructible) so it is the
        // variant's default state.
        struct AstNode {
            std::variant<
                Nil, Skip,
                True, False, IntLit, FltLit, StrLit, Vararg,
                Name, BinOp, UnOp, Paren, Index, Field, Call, MethodCall,
                TableCtor, FieldEntry, FuncBody, FuncDef,
                Block, Chunk, Assign, CallStat, Do, While, Repeat,
                If, NumericFor, GenericFor, LocalDecl, LocalFunction,
                FuncStat, FuncName, Goto, Label, Break, Return
            > v;

            AstNode() = default;
            // Move-only: alternatives hold Box (unique_ptr), so copies are
            // ill-formed. Actions move values out of the parse tree.
            AstNode(const AstNode&) = delete;
            AstNode& operator=(const AstNode&) = delete;
            AstNode(AstNode&&) noexcept = default;
            AstNode& operator=(AstNode&&) noexcept = default;

            // Converting constructor for a single alternative. Excludes
            // AstNode itself so it does not shadow the move constructor.
            template<typename T>
                requires (!std::is_same_v<std::remove_cvref_t<T>, AstNode>)
            AstNode(T&& x) : v(std::forward<T>(x)) {}

            // Source span accessors. Every alternative has start/end members.
            std::size_t start() const
            {
                return std::visit([](const auto& x) -> std::size_t { return x.start; },
                                  v);
            }
            std::size_t end() const
            {
                return std::visit([](const auto& x) -> std::size_t { return x.end; },
                                  v);
            }

            std::size_t index() const noexcept { return v.index(); }
        };

        // Box(AstNode): heap-allocate and own. Defined here, after AstNode.
        inline Box::Box(AstNode x)
            : p(std::make_unique<AstNode>(std::move(x))) {}

        template<typename T> bool holds(const AstNode& n) noexcept {
            return std::holds_alternative<T>(n.v);
        }
        template<typename T> T& get(AstNode& n) { return std::get<T>(n.v); }
        template<typename T> const T& get(const AstNode& n) { return std::get<T>(n.v); }

    } // namespace lua
} // namespace ys
