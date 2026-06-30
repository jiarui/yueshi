#pragma once
// ---------------------------------------------------------------------------
// Navigation & predicate helpers over the typed AstNode variant, used by the
// structural parser tests (parser_structure.cpp).
//
// Header-only and free of any doctest dependency, so it just provides typed
// views over the tree. Every accessor returns a nullable pointer: returning
// nullptr (rather than asserting) keeps a failure localized to the CHECK that
// actually cares, and lets a test distinguish "wrong node kind" from "wrong
// field" cleanly. The usual idiom is:
//
//     auto* e = ret("1 + 2 * 3");          // the single return expression
//     REQUIRE(is_binop(*e, BinOpKind::Add));
//     REQUIRE(is_binop(*binop_rhs(*e), BinOpKind::Mul));
// ---------------------------------------------------------------------------

#include "lua/ast.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ys::lua::test_ast {

// --- core ------------------------------------------------------------------

// Typed view of a node, or nullptr if it holds a different alternative.
template <typename T>
const T* as(const AstNode& n) { return std::get_if<T>(&n.v); }

// Dereference a Box to its node, or nullptr if the Box is empty.
inline const AstNode* raw(const Box& b) { return b ? b.operator->() : nullptr; }

// --- chunk / block ---------------------------------------------------------

inline const Chunk* chunk_of(const AstNode& root) { return as<Chunk>(root); }

inline const Block* block_of(const AstNode& root) {
    if (auto* c = as<Chunk>(root)) return &c->body;
    return nullptr;
}

inline const std::vector<Box>* stmts_of(const AstNode& root) {
    if (auto* b = block_of(root)) return &b->stmts;
    return nullptr;
}

// The i-th top-level statement of a chunk, or nullptr.
inline const AstNode* stmt_at(const AstNode& root, std::size_t i) {
    if (auto* s = stmts_of(root))
        return i < s->size() ? raw((*s)[i]) : nullptr;
    return nullptr;
}

// --- literals & atoms ------------------------------------------------------

inline bool is_nil(const AstNode& n)    { return as<Nil>(n) != nullptr; }
inline bool is_true(const AstNode& n)   { return as<True>(n) != nullptr; }
inline bool is_false(const AstNode& n)  { return as<False>(n) != nullptr; }
inline bool is_vararg(const AstNode& n) { return as<Vararg>(n) != nullptr; }

inline const IntLit* intlit(const AstNode& n) { return as<IntLit>(n); }
inline const FltLit* fltlit(const AstNode& n) { return as<FltLit>(n); }
inline const StrLit* strlit(const AstNode& n) { return as<StrLit>(n); }
inline const Name*   name(const AstNode& n)   { return as<Name>(n); }

inline bool is_name(const AstNode& n, std::string_view s) {
    if (auto* nm = as<Name>(n)) return nm->name == s;
    return false;
}

// --- operators -------------------------------------------------------------

inline const BinOp* binop(const AstNode& n) { return as<BinOp>(n); }
inline bool is_binop(const AstNode& n, BinOpKind k) {
    if (auto* b = as<BinOp>(n)) return b->op == k;
    return false;
}
inline const AstNode* binop_lhs(const AstNode& n) {
    if (auto* b = as<BinOp>(n)) return raw(b->lhs);
    return nullptr;
}
inline const AstNode* binop_rhs(const AstNode& n) {
    if (auto* b = as<BinOp>(n)) return raw(b->rhs);
    return nullptr;
}

inline const UnOp* unop(const AstNode& n) { return as<UnOp>(n); }
inline bool is_unop(const AstNode& n, UnOpKind k) {
    if (auto* u = as<UnOp>(n)) return u->op == k;
    return false;
}
inline const AstNode* unop_operand(const AstNode& n) {
    if (auto* u = as<UnOp>(n)) return raw(u->operand);
    return nullptr;
}

// --- prefixexp / suffix folds ----------------------------------------------

inline const Paren* paren(const AstNode& n) { return as<Paren>(n); }
inline const AstNode* paren_inner(const AstNode& n) {
    if (auto* p = as<Paren>(n)) return raw(p->exp);
    return nullptr;
}

inline const Index* index(const AstNode& n) { return as<Index>(n); }
inline const AstNode* index_obj(const AstNode& n) {
    if (auto* x = as<Index>(n)) return raw(x->obj);
    return nullptr;
}
inline const AstNode* index_key(const AstNode& n) {
    if (auto* x = as<Index>(n)) return raw(x->key);
    return nullptr;
}

inline const Field* field(const AstNode& n) { return as<Field>(n); }
inline const AstNode* field_obj(const AstNode& n) {
    if (auto* x = as<Field>(n)) return raw(x->obj);
    return nullptr;
}
inline const std::string* field_name(const AstNode& n) {
    if (auto* x = as<Field>(n)) return &x->name;
    return nullptr;
}

inline const Call* call(const AstNode& n) { return as<Call>(n); }
inline const AstNode* call_func(const AstNode& n) {
    if (auto* x = as<Call>(n)) return raw(x->func);
    return nullptr;
}
inline const std::vector<Box>* call_args(const AstNode& n) {
    if (auto* x = as<Call>(n)) return &x->args;
    return nullptr;
}

inline const MethodCall* methodcall(const AstNode& n) { return as<MethodCall>(n); }
inline const AstNode* methodcall_obj(const AstNode& n) {
    if (auto* x = as<MethodCall>(n)) return raw(x->obj);
    return nullptr;
}
inline const std::string* methodcall_name(const AstNode& n) {
    if (auto* x = as<MethodCall>(n)) return &x->method;
    return nullptr;
}
inline const std::vector<Box>* methodcall_args(const AstNode& n) {
    if (auto* x = as<MethodCall>(n)) return &x->args;
    return nullptr;
}

// --- tables ----------------------------------------------------------------

inline const TableCtor* tablector(const AstNode& n) { return as<TableCtor>(n); }
inline const std::vector<FieldEntry>* table_fields(const AstNode& n) {
    if (auto* x = as<TableCtor>(n)) return &x->fields;
    return nullptr;
}

// --- functions -------------------------------------------------------------

// NOTE: there is no funcbody()/funcbody_params()/funcbody_block() accessor
// here. No AstNode ever holds a bare FuncBody as its variant alternative in a
// position tests need to navigate: FuncDef / LocalFunction / FuncStat all
// embed FuncBody as a *direct struct member*, so tests reach its fields via
// funcdef(...)->body, localfunction(...)->body, funcstat(...)->body. An
// earlier version of this header defined FuncBody-taking accessors that
// required converting a move-only struct into an AstNode (ill-formed) and
// caused hard compile errors at the call sites.

inline const FuncDef* funcdef(const AstNode& n) { return as<FuncDef>(n); }

// --- statements ------------------------------------------------------------

inline const Assign* assign(const AstNode& n) { return as<Assign>(n); }
inline const std::vector<Box>* assign_targets(const AstNode& n) {
    if (auto* x = as<Assign>(n)) return &x->targets;
    return nullptr;
}
inline const std::vector<Box>* assign_values(const AstNode& n) {
    if (auto* x = as<Assign>(n)) return &x->values;
    return nullptr;
}

inline const CallStat* callstat(const AstNode& n) { return as<CallStat>(n); }
inline const AstNode* callstat_call(const AstNode& n) {
    if (auto* x = as<CallStat>(n)) return raw(x->call);
    return nullptr;
}

inline const Do* do_stat(const AstNode& n) { return as<Do>(n); }
inline const AstNode* do_body(const AstNode& n) {
    if (auto* x = as<Do>(n)) return raw(x->body);
    return nullptr;
}

inline const While* while_stat(const AstNode& n) { return as<While>(n); }
inline const AstNode* while_cond(const AstNode& n) {
    if (auto* x = as<While>(n)) return raw(x->cond);
    return nullptr;
}
inline const AstNode* while_body(const AstNode& n) {
    if (auto* x = as<While>(n)) return raw(x->body);
    return nullptr;
}

inline const Repeat* repeat_stat(const AstNode& n) { return as<Repeat>(n); }
inline const AstNode* repeat_body(const AstNode& n) {
    if (auto* x = as<Repeat>(n)) return raw(x->body);
    return nullptr;
}
inline const AstNode* repeat_cond(const AstNode& n) {
    if (auto* x = as<Repeat>(n)) return raw(x->cond);
    return nullptr;
}

inline const If* if_stat(const AstNode& n) { return as<If>(n); }
inline const std::vector<std::pair<Box, Box>>* if_branches(const AstNode& n) {
    if (auto* x = as<If>(n)) return &x->branches;
    return nullptr;
}
inline const AstNode* if_else(const AstNode& n) {
    if (auto* x = as<If>(n)) return x->else_body ? raw(*x->else_body) : nullptr;
    return nullptr;
}

inline const NumericFor* numfor(const AstNode& n) { return as<NumericFor>(n); }
inline const GenericFor* genfor(const AstNode& n) { return as<GenericFor>(n); }

inline const LocalDecl* localdecl(const AstNode& n) { return as<LocalDecl>(n); }
inline const LocalFunction* localfunction(const AstNode& n) { return as<LocalFunction>(n); }
inline const FuncStat* funcstat(const AstNode& n) { return as<FuncStat>(n); }
inline const Goto* goto_stat(const AstNode& n) { return as<Goto>(n); }
inline const Label* label_stat(const AstNode& n) { return as<Label>(n); }
inline const Break* break_stat(const AstNode& n) { return as<Break>(n); }

inline const Return* return_stat(const AstNode& n) { return as<Return>(n); }
inline const std::vector<Box>* return_values(const AstNode& n) {
    if (auto* x = as<Return>(n)) return x->values ? &*x->values : nullptr;
    return nullptr;
}

} // namespace ys::lua::test_ast
