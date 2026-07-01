#include "lua/goto_check.h"

#include <string>
#include <variant>
#include <vector>

#include "lua/evaluator.h"   // LuaError

namespace ys
{
    namespace lua
    {
        namespace {

        // -------------------------------------------------------------------
        // offset_to_line: convert a byte offset into a 1-based line number.
        // -------------------------------------------------------------------
        std::size_t offset_to_line(std::string_view src, std::size_t off)
        {
            std::size_t line = 1;
            for (std::size_t i = 0; i < off && i < src.size(); ++i)
                if (src[i] == '\n') ++line;
            return line;
        }

        // -------------------------------------------------------------------
        // Check state: the per-function label/goto/local tracking.
        // -------------------------------------------------------------------
        struct LabelDesc {
            std::string name;
            std::size_t nactvar;   // active-local count at the label position
            std::size_t off;       // source offset (for error messages)
        };

        struct GotoDesc {
            std::string label;     // target label name
            std::size_t nactvar;   // active-local count at the goto position
            std::size_t off;       // source offset
        };

        struct CheckState {
            std::vector<LabelDesc>  labels;        // active labels (function-level)
            std::vector<GotoDesc>   pending;       // unresolved forward gotos
            std::vector<std::string> local_names;  // local var names (function-level, indexed by nactvar)
            std::size_t             nactvar{0};    // current active-local count
            std::size_t             loop_depth{0}; // for break validation
            std::string_view        source;        // for offset→line conversion
        };

        // Forward declarations.
        void check_block(const Block& blk, CheckState& cs,
                         bool is_function_top, bool is_repeat_body);
        void check_function(const FuncBody& fb, CheckState& cs);
        void walk_expr(const AstNode& expr, CheckState& cs);

        // -------------------------------------------------------------------
        // is_last_nonvoid: true if all statements after index `i` are void
        // (null Box, Label, or Skip). Used for the end-of-block exemption.
        // -------------------------------------------------------------------
        bool is_last_nonvoid(const Block& blk, std::size_t i)
        {
            for (std::size_t j = i + 1; j < blk.stmts.size(); ++j) {
                if (!blk.stmts[j]) continue;            // null = void
                const AstNode& s = *blk.stmts[j];
                if (holds<Label>(s) || holds<Skip>(s)) continue;
                return false;                           // a real statement follows
            }
            return true;
        }

        // -------------------------------------------------------------------
        // find_label: search visible labels (function-level) for a name.
        // Returns pointer to the LabelDesc, or nullptr.
        // -------------------------------------------------------------------
        LabelDesc* find_label(CheckState& cs, std::string_view name)
        {
            for (auto& lb : cs.labels)
                if (lb.name == name)
                    return &lb;
            return nullptr;
        }

        // -------------------------------------------------------------------
        // R2 check: does the goto jump into the scope of a local?
        // -------------------------------------------------------------------
        void check_jump_scope(CheckState& cs, const GotoDesc& gt,
                              const LabelDesc& lb)
        {
            if (gt.nactvar < lb.nactvar) {
                // The offending variable is at index gt.nactvar — the first
                // local that the goto would enter.
                std::string varname =
                    (gt.nactvar < cs.local_names.size())
                        ? cs.local_names[gt.nactvar] : "?";
                throw LuaError(
                    "<goto " + gt.label + "> at line " +
                    std::to_string(offset_to_line(cs.source, gt.off)) +
                    " jumps into the scope of local '" + varname + "'",
                    gt.off);
            }
        }

        // -------------------------------------------------------------------
        // resolve_pending: when a label is declared, resolve any pending
        // gotos THAT BELONG TO THE SAME BLOCK (index >= firstgoto). A label
        // in a nested block must NOT resolve gotos from an enclosing block.
        // -------------------------------------------------------------------
        void resolve_pending(CheckState& cs, const LabelDesc& lb,
                             std::size_t firstgoto)
        {
            for (std::size_t i = firstgoto; i < cs.pending.size(); ) {
                if (cs.pending[i].label == lb.name) {
                    check_jump_scope(cs, cs.pending[i], lb);
                    cs.pending.erase(cs.pending.begin() +
                                     static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }

        // -------------------------------------------------------------------
        // check_block: the core analysis for a single block.
        // -------------------------------------------------------------------
        void check_block(const Block& blk, CheckState& cs,
                         bool is_function_top, bool is_repeat_body)
        {
            std::size_t base_nactvar  = cs.nactvar;
            std::size_t firstlabel    = cs.labels.size();
            std::size_t firstgoto     = cs.pending.size();
            (void)firstlabel;  // used implicitly via cs.labels.resize below

            for (std::size_t i = 0; i < blk.stmts.size(); ++i) {
                if (!blk.stmts[i]) continue;
                const AstNode& stmt = *blk.stmts[i];

                if (holds<Label>(stmt)) {
                    const Label& lbl = get<Label>(stmt);
                    // R3: duplicate visible label.
                    if (find_label(cs, lbl.name))
                        throw LuaError(
                            "label '" + lbl.name + "' already defined",
                            lbl.start);
                    // End-of-block exemption: a label that is the last
                    // non-void statement gets nactvar = enclosing scope.
                    // Repeat body labels NEVER get the exemption.
                    bool at_end = !is_repeat_body && is_last_nonvoid(blk, i);
                    LabelDesc ld;
                    ld.name    = lbl.name;
                    ld.nactvar = at_end ? base_nactvar : cs.nactvar;
                    ld.off     = lbl.start;
                    cs.labels.push_back(ld);
                    resolve_pending(cs, ld, firstgoto);
                    continue;
                }

                if (holds<Goto>(stmt)) {
                    const Goto& g = get<Goto>(stmt);
                    LabelDesc* target = find_label(cs, g.label);
                    if (target) {
                        // Backward jump: resolve immediately.
                        GotoDesc gd;
                        gd.label  = g.label;
                        gd.nactvar = cs.nactvar;
                        gd.off     = g.start;
                        check_jump_scope(cs, gd, *target);
                    } else {
                        // Forward jump: defer.
                        GotoDesc gd;
                        gd.label  = g.label;
                        gd.nactvar = cs.nactvar;
                        gd.off     = g.start;
                        cs.pending.push_back(gd);
                    }
                    continue;
                }

                if (holds<Break>(stmt)) {
                    if (cs.loop_depth == 0)
                        throw LuaError(
                            "break outside loop at line " +
                            std::to_string(
                                offset_to_line(cs.source, stmt.start())),
                            stmt.start());
                    continue;
                }

                // Handle scope-introducing and local-declaring statements.
                auto visit = [&](const auto& x) {
                    using T = std::remove_cvref_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, LocalDecl>) {
                        // Walk values for nested functions BEFORE pushing locals
                        // (the new locals are NOT in scope for their own init).
                        if (x.values)
                            for (const auto& v : *x.values)
                                if (v) walk_expr(*v, cs);
                        // Push locals.
                        for (const auto& name : x.names) {
                            cs.local_names.push_back(name);
                            cs.nactvar++;
                        }
                    }
                    else if constexpr (std::is_same_v<T, LocalFunction>) {
                        // local function f() ... end — f is in scope for its
                        // own body (Lua desugars to: local f; f = function...
                        cs.local_names.push_back(x.name);
                        cs.nactvar++;
                        check_function(x.body, cs);
                    }
                    else if constexpr (std::is_same_v<T, FuncStat>) {
                        // function a.b.c:d() ... end — check the body.
                        // The dotted name is NOT a local; it's a global/table
                        // field assignment. Only the function body matters.
                        check_function(x.body, cs);
                    }
                    else if constexpr (std::is_same_v<T, Do>) {
                        if (x.body)
                            check_block(get<Block>(*x.body), cs, false, false);
                    }
                    else if constexpr (std::is_same_v<T, While>) {
                        if (x.cond) walk_expr(*x.cond, cs);
                        ++cs.loop_depth;
                        if (x.body)
                            check_block(get<Block>(*x.body), cs, false, false);
                        --cs.loop_depth;
                    }
                    else if constexpr (std::is_same_v<T, Repeat>) {
                        // Repeat: the cond is evaluated in the body's scope.
                        // A label at the end of the body does NOT get the
                        // end-of-block exemption (is_repeat_body = true).
                        ++cs.loop_depth;
                        if (x.body)
                            check_block(get<Block>(*x.body), cs, false, true);
                        // Walk cond in the body's extended scope (body locals
                        // are in scope at the until condition).
                        if (x.cond) walk_expr(*x.cond, cs);
                        --cs.loop_depth;
                        // Pop body locals (done by check_block exit logic).
                    }
                    else if constexpr (std::is_same_v<T, NumericFor>) {
                        if (x.init)  walk_expr(*x.init, cs);
                        if (x.limit) walk_expr(*x.limit, cs);
                        if (x.step && *x.step) walk_expr(**x.step, cs);
                        // Loop variable is a new local for the body.
                        cs.local_names.push_back(x.var);
                        cs.nactvar++;
                        ++cs.loop_depth;
                        if (x.body)
                            check_block(get<Block>(*x.body), cs, false, false);
                        --cs.loop_depth;
                    }
                    else if constexpr (std::is_same_v<T, GenericFor>) {
                        for (const auto& e : x.exprs)
                            if (e) walk_expr(*e, cs);
                        for (const auto& name : x.names) {
                            cs.local_names.push_back(name);
                            cs.nactvar++;
                        }
                        ++cs.loop_depth;
                        if (x.body)
                            check_block(get<Block>(*x.body), cs, false, false);
                        --cs.loop_depth;
                    }
                    else if constexpr (std::is_same_v<T, If>) {
                        for (const auto& [cond, body] : x.branches) {
                            if (cond) walk_expr(*cond, cs);
                            if (body)
                                check_block(get<Block>(*body), cs, false, false);
                        }
                        if (x.else_body && *x.else_body)
                            check_block(get<Block>(**x.else_body), cs, false, false);
                    }
                    else if constexpr (std::is_same_v<T, Return>) {
                        if (x.values)
                            for (const auto& v : *x.values)
                                if (v) walk_expr(*v, cs);
                    }
                    else if constexpr (std::is_same_v<T, Assign>) {
                        // Walk both targets and values for nested functions.
                        for (const auto& t : x.targets)
                            if (t) walk_expr(*t, cs);
                        for (const auto& v : x.values)
                            if (v) walk_expr(*v, cs);
                    }
                    else if constexpr (std::is_same_v<T, CallStat>) {
                        if (x.call) walk_expr(*x.call, cs);
                    }
                    else {
                        // Skip, Nil, etc. — no sub-expressions to check.
                    }
                };
                std::visit(visit, stmt.v);
            }

            // Migrate pending gotos: adjust their nactvar to the block's base.
            for (std::size_t i = firstgoto; i < cs.pending.size(); ++i) {
                if (cs.pending[i].nactvar > base_nactvar)
                    cs.pending[i].nactvar = base_nactvar;
            }

            // Function-top: any unresolved goto = R1 error.
            if (is_function_top && cs.pending.size() > firstgoto) {
                const GotoDesc& gd = cs.pending.back();
                throw LuaError(
                    "no visible label '" + gd.label + "' for <goto> at line " +
                    std::to_string(offset_to_line(cs.source, gd.off)),
                    gd.off);
            }

            // Pop this block's labels.
            cs.labels.resize(firstlabel);
            // Pop this block's locals.
            cs.local_names.resize(
                cs.local_names.size() - (cs.nactvar - base_nactvar));
            cs.nactvar = base_nactvar;
        }

        // -------------------------------------------------------------------
        // check_function: analyze a function body with a fresh label/goto space.
        // -------------------------------------------------------------------
        void check_function(const FuncBody& fb, CheckState& cs)
        {
            // Save state (the caller's function context).
            auto saved_labels      = std::move(cs.labels);
            auto saved_pending     = std::move(cs.pending);
            auto saved_local_names = std::move(cs.local_names);
            auto saved_nactvar     = cs.nactvar;
            auto saved_loop_depth  = cs.loop_depth;

            cs.labels.clear();
            cs.pending.clear();
            cs.local_names.clear();
            cs.nactvar = 0;
            cs.loop_depth = 0;

            // Parameters are active locals from function entry.
            for (const auto& p : fb.params) {
                if (p.kind == Param::Kind::Name) {
                    cs.local_names.push_back(p.name);
                    cs.nactvar++;
                }
            }

            // Check the body (function-top block).
            if (fb.body)
                check_block(get<Block>(*fb.body), cs, true, false);

            // Restore state.
            cs.labels      = std::move(saved_labels);
            cs.pending     = std::move(saved_pending);
            cs.local_names = std::move(saved_local_names);
            cs.nactvar     = saved_nactvar;
            cs.loop_depth  = saved_loop_depth;
        }

        // -------------------------------------------------------------------
        // walk_expr: recursively walk an expression tree, checking any
        // function definitions (FuncDef → FuncBody) found within.
        // -------------------------------------------------------------------
        void walk_expr(const AstNode& expr, CheckState& cs)
        {
            std::visit([&](const auto& x) {
                using T = std::remove_cvref_t<decltype(x)>;
                if constexpr (std::is_same_v<T, FuncDef>) {
                    check_function(x.body, cs);
                }
                else if constexpr (std::is_same_v<T, BinOp>) {
                    if (x.lhs) walk_expr(*x.lhs, cs);
                    if (x.rhs) walk_expr(*x.rhs, cs);
                }
                else if constexpr (std::is_same_v<T, UnOp>) {
                    if (x.operand) walk_expr(*x.operand, cs);
                }
                else if constexpr (std::is_same_v<T, Paren>) {
                    if (x.exp) walk_expr(*x.exp, cs);
                }
                else if constexpr (std::is_same_v<T, Index>) {
                    if (x.obj) walk_expr(*x.obj, cs);
                    if (x.key) walk_expr(*x.key, cs);
                }
                else if constexpr (std::is_same_v<T, Call>) {
                    if (x.func) walk_expr(*x.func, cs);
                    for (const auto& a : x.args)
                        if (a) walk_expr(*a, cs);
                }
                else if constexpr (std::is_same_v<T, MethodCall>) {
                    if (x.obj) walk_expr(*x.obj, cs);
                    for (const auto& a : x.args)
                        if (a) walk_expr(*a, cs);
                }
                else if constexpr (std::is_same_v<T, TableCtor>) {
                    for (const auto& fe : x.fields) {
                        if (fe.key) walk_expr(*fe.key, cs);
                        if (fe.value) walk_expr(*fe.value, cs);
                    }
                }
                else {
                    // Leaves: Nil, Skip, True, False, IntLit, FltLit, StrLit,
                    // Vararg, Name, Field, FieldEntry, FuncName, etc.
                }
            }, expr.v);
        }

        } // anonymous namespace

        // -------------------------------------------------------------------
        // Public entry point.
        // -------------------------------------------------------------------
        void check_goto_scopes(const AstNode& root, std::string_view source)
        {
            if (!holds<Chunk>(root)) return;
            const Chunk& chunk = get<Chunk>(root);
            CheckState cs;
            cs.source = source;
            // The main chunk is the top-level function body. Check it
            // with a fresh label/goto space.
            check_block(chunk.body, cs, true, false);
        }

    } // namespace lua
} // namespace ys
