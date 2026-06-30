#include "lua/evaluator.h"

#include <cstdlib>
#include <ostream>
#include <utility>
#include <variant>

#include "lua/ast.h"
#include "lua/numops.h"

namespace ys
{
    namespace lua
    {
        // -------------------------------------------------------------------
        // Construction + the public entry point
        // -------------------------------------------------------------------
        Evaluator::Evaluator(Heap& heap, std::ostream& out)
            : m_heap(heap), m_out(&out)
        {
            m_globals = m_heap.make_env(nullptr);
            install_builtins();
        }

        ValueVec Evaluator::run(AstNode root)
        {
            // The chunk root is a Chunk{ Block body }.
            if (!holds<Chunk>(root))
                throw LuaError("expected a chunk at the AST root", root.start());

            Chunk& chunk = get<Chunk>(root);
            // The main chunk is a vararg function. In M2.0 its `...` is empty
            // (no caller), so we leave globals()->varargs default-empty.
            Control c = eval_block(chunk.body, m_globals);
            if (c.flow == Flow::Return)
                return std::move(c.vals);
            return {};
        }

        // -------------------------------------------------------------------
        // Builtins. Each is a static function with the BuiltinFn signature;
        // install_builtins() registers them into globals(). The iterator family
        // (pairs/ipairs/next) drives the generic-for loop.
        // -------------------------------------------------------------------
        namespace builtins {

        ValueVec b_print(Evaluator& ev, ValueVec args)
        {
            std::string sep;
            for (const LuaValue& v : args) {
                ev.out() << sep << value_to_string(v);
                sep = "\t";
            }
            ev.out() << "\n";
            return {};
        }

        ValueVec b_type(Evaluator& ev, ValueVec args)
        {
            const LuaValue& v = args.empty() ? LuaValue::nil() : args[0];
            return {LuaValue::str(ev.heap().make_string(type_name(v)))};
        }

        ValueVec b_tostring(Evaluator& ev, ValueVec args)
        {
            LuaValue v = args.empty() ? LuaValue::nil() : args[0];
            return {LuaValue::str(ev.heap().make_string(value_to_string(v)))};
        }

        ValueVec b_tonumber(Evaluator& ev, ValueVec args)
        {
            // tonumber(v [, base]). Base form (base==10): accept a number or a
            // numeric string; else return nil. (Base conversion is M3.)
            if (args.empty()) return {LuaValue::nil()};
            const LuaValue& v = args[0];
            if (v.is_number()) return {v};
            if (v.is_str()) {
                const std::string& s = v.as_str()->data;
                // Try integer, then float.
                char* end = nullptr;
                long long li = std::strtoll(s.c_str(), &end, 10);
                if (end != s.c_str() && *end == '\0')
                    return {LuaValue::integer(li)};
                end = nullptr;
                double d = std::strtod(s.c_str(), &end);
                if (end != s.c_str() && *end == '\0')
                    return {LuaValue::flt(d)};
            }
            (void)ev;
            return {LuaValue::nil()};
        }

        ValueVec b_error(Evaluator&, ValueVec args)
        {
            // error(v) throws. M2.0: no error objects / level; just the message.
            LuaValue v = args.empty() ? LuaValue::nil() : args[0];
            throw LuaError(v.is_str() ? v.as_str()->data
                                      : value_to_string(v), 0);
        }

        ValueVec b_assert(Evaluator&, ValueVec args)
        {
            if (!args.empty() && args[0].truthy()) return args;   // pass through
            std::string msg = args.size() >= 2 ? value_to_string(args[1])
                                               : "assertion failed!";
            throw LuaError(msg, 0);
        }

        // ipairs(t) -> iterator, t, 0. The iterator is a builtin that, given
        // (t, i), returns t[i+1] or nil.
        ValueVec b_ipairs_iter(Evaluator&, ValueVec args)
        {
            LuaValue t = args.empty() ? LuaValue::nil() : args[0];
            long long i = args.size() >= 2 && args[1].is_int() ? args[1].as_int() : 0;
            ++i;
            if (!t.is_table()) return {LuaValue::nil()};
            LuaKey k; k.k = LuaKey::K::Int; k.i = i;
            auto it = t.as_table()->hash.find(k);
            if (it == t.as_table()->hash.end() || it->second.is_nil())
                return {LuaValue::nil()};
            return {LuaValue::integer(i), it->second};
        }

        ValueVec b_ipairs(Evaluator& ev, ValueVec args)
        {
            LuaValue t = args.empty() ? LuaValue::nil() : args[0];
            Builtin* iter = ev.heap().make_builtin("ipairs_iter", b_ipairs_iter);
            return {LuaValue::builtin(iter), t, LuaValue::integer(0)};
        }

        // pairs(t) -> next, t, nil. next enumerates all keys (hash order).
        // next(t, k) returns the (key, value) pair following k, or nil if k was
        // the last. With k == nil, returns the first pair.
        ValueVec b_next(Evaluator& ev, ValueVec args)
        {
            LuaValue t = args.empty() ? LuaValue::nil() : args[0];
            LuaValue k = args.size() >= 2 ? args[1] : LuaValue::nil();
            if (!t.is_table()) return {LuaValue::nil()};
            const auto& hash = t.as_table()->hash;
            if (hash.empty()) return {LuaValue::nil()};

            // Materialize a map key back into a LuaValue. String keys need a
            // heap allocation; we use the evaluator's heap.
            auto keyval = [&](const LuaKey& kk) -> LuaValue {
                switch (kk.k) {
                case LuaKey::K::Int:  return LuaValue::integer(kk.i);
                case LuaKey::K::Flt:  return LuaValue::flt(kk.f);
                case LuaKey::K::Str:  return LuaValue::str(
                    ev.heap().make_string(kk.s));
                case LuaKey::K::Bool: return LuaValue::boolean(kk.b);
                case LuaKey::K::Ptr:  return LuaValue::nil();  // identity keys rare
                }
                return LuaValue::nil();
            };

            auto emit_pair = [&](auto it) -> ValueVec {
                return {keyval(it->first), it->second};
            };

            if (k.is_nil()) return emit_pair(hash.begin());

            // Find the given key, return the following one. O(n) per step, but
            // iteration order is stable as long as the table isn't mutated
            // during the loop (Lua forbids that anyway).
            LuaKey cur;
            if (!to_key(k, cur)) return {LuaValue::nil()};
            auto it = hash.find(cur);
            if (it == hash.end()) return {LuaValue::nil()};
            ++it;
            if (it == hash.end()) return {LuaValue::nil()};
            return emit_pair(it);
        }

        ValueVec b_pairs(Evaluator& ev, ValueVec args)
        {
            LuaValue t = args.empty() ? LuaValue::nil() : args[0];
            Builtin* n = ev.heap().make_builtin("next", b_next);
            return {LuaValue::builtin(n), t, LuaValue::nil()};
        }

        ValueVec b_select(Evaluator&, ValueVec args)
        {
            if (args.empty()) throw LuaError("bad argument #1 to 'select'", 0);
            const LuaValue& n = args[0];
            if (n.is_str() && n.as_str()->data == "#")
                return {LuaValue::integer(
                    static_cast<long long>(args.size() - 1))};
            if (!n.is_int()) throw LuaError("bad argument #1 to 'select'", 0);
            long long i = n.as_int();
            if (i < 0) i = static_cast<long long>(args.size()) + i;   // from end
            if (i < 1) throw LuaError("bad argument #1 to 'select' (index out of range)", 0);
            ValueVec out;
            for (std::size_t j = static_cast<std::size_t>(i); j < args.size(); ++j)
                out.push_back(args[j]);
            return out;
        }

        ValueVec b_rawget(Evaluator&, ValueVec args)
        {
            if (args.empty() || !args[0].is_table()) return {LuaValue::nil()};
            LuaValue t = args[0], k = args.size() >= 2 ? args[1] : LuaValue::nil();
            LuaKey kk;
            if (!to_key(k, kk)) return {LuaValue::nil()};
            auto it = t.as_table()->hash.find(kk);
            return {it == t.as_table()->hash.end() ? LuaValue::nil() : it->second};
        }

        ValueVec b_rawset(Evaluator&, ValueVec args)
        {
            if (args.empty() || !args[0].is_table())
                throw LuaError("bad argument #1 to 'rawset' (table expected)", 0);
            LuaValue t = args[0];
            LuaValue k = args.size() >= 2 ? args[1] : LuaValue::nil();
            LuaValue v = args.size() >= 3 ? args[2] : LuaValue::nil();
            LuaKey kk;
            if (!to_key(k, kk)) {
                if (k.is_nil()) return {t};
                throw LuaError("table index is NaN", 0);
            }
            if (v.is_nil()) t.as_table()->hash.erase(kk);
            else            t.as_table()->hash[kk] = v;
            return {t};
        }

        ValueVec b_rawequal(Evaluator&, ValueVec args)
        {
            LuaValue a = args.size() >= 1 ? args[0] : LuaValue::nil();
            LuaValue b = args.size() >= 2 ? args[1] : LuaValue::nil();
            return {LuaValue::boolean(raw_equal(a, b))};
        }

        ValueVec b_rawlen(Evaluator&, ValueVec args)
        {
            LuaValue v = args.empty() ? LuaValue::nil() : args[0];
            if (v.is_str())
                return {LuaValue::integer(
                    static_cast<long long>(v.as_str()->data.size()))};
            if (v.is_table())
                return {LuaValue::integer(table_border(v.as_table()))};
            throw LuaError("table or string expected", 0);
        }

        } // namespace builtins

        void Evaluator::install_builtins()
        {
            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = m_heap.make_builtin(name, fn);
                m_globals->vars[name] = LuaValue::builtin(b);
            };
            add("print",    builtins::b_print);
            add("type",     builtins::b_type);
            add("tostring", builtins::b_tostring);
            add("tonumber", builtins::b_tonumber);
            add("error",    builtins::b_error);
            add("assert",   builtins::b_assert);
            add("ipairs",   builtins::b_ipairs);
            add("pairs",    builtins::b_pairs);
            add("next",     builtins::b_next);
            add("select",   builtins::b_select);
            add("rawget",   builtins::b_rawget);
            add("rawset",   builtins::b_rawset);
            add("rawequal", builtins::b_rawequal);
            add("rawlen",   builtins::b_rawlen);
        }

        // -------------------------------------------------------------------
        // Block + statement evaluation
        // -------------------------------------------------------------------
        Control Evaluator::eval_block(const Block& blk, Environment* env)
        {
            for (const Box& s : blk.stmts) {
                if (!s) continue;
                Control c = eval_stat(*s, env);
                if (c.flow != Flow::Normal)
                    return c;   // break/return propagates out of the block
            }
            return {};   // Normal
        }

        Control Evaluator::eval_stat(const AstNode& node, Environment* env)
        {
            return std::visit(
                [&](const auto& x) -> Control {
                    using T = std::remove_cvref_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, LocalDecl>) {
                        // local namelist [= explist]
                        // Evaluate the RHS as an explist (multires on the last
                        // expr); bind each name to the corresponding value,
                        // defaulting missing ones to nil.
                        ValueVec vals = x.values ? eval_explist(*x.values, env)
                                                 : ValueVec{};
                        for (std::size_t i = 0; i < x.names.size(); ++i) {
                            LuaValue v = i < vals.size() ? vals[i] : LuaValue::nil();
                            env->vars[x.names[i]] = v;
                            // Record <const> attribute for later enforcement.
                            if (i < x.attribs.size() &&
                                x.attribs[i] == Attrib::Const) {
                                env->consts.insert(x.names[i]);
                            }
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, Return>) {
                        Control c;
                        c.flow = Flow::Return;
                        if (x.values)
                            c.vals = eval_explist(*x.values, env);
                        return c;
                    }
                    else if constexpr (std::is_same_v<T, Assign>) {
                        // varlist '=' explist. Targets are lvalues (Name/Index/
                        // Field). Evaluate all RHS values first (Lua semantics:
                        // all RHS evaluated before any assignment).
                        ValueVec vals = eval_explist(x.values, env);
                        for (std::size_t i = 0; i < x.targets.size(); ++i) {
                            LuaValue v = i < vals.size() ? vals[i] : LuaValue::nil();
                            assign_target(*x.targets[i], env, v, node.start());
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, Do>) {
                        // A block introduces a new scope.
                        Environment* scope = m_heap.make_env(env);
                        return eval_block_box(x.body, scope);
                    }
                    else if constexpr (std::is_same_v<T, While>) {
                        while (eval_scalar(*x.cond, env).truthy()) {
                            Control c = eval_block_box(x.body, env);
                            if (c.flow == Flow::Break)    break;
                            if (c.flow == Flow::Return)   return c;
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, Repeat>) {
                        // repeat ... until cond: the body sees the condition's
                        // scope (cond may name a local declared in the body).
                        while (true) {
                            Environment* scope = m_heap.make_env(env);
                            Control c = eval_block_box(x.body, scope);
                            if (c.flow == Flow::Break)    break;
                            if (c.flow == Flow::Return)   return c;
                            // cond is evaluated in the scope that includes the
                            // body's locals (Lua §3.3.5).
                            if (eval_scalar(*x.cond, scope).truthy()) break;
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, If>) {
                        for (const auto& [cond, body] : x.branches) {
                            if (eval_scalar(*cond, env).truthy()) {
                                Environment* scope = m_heap.make_env(env);
                                return eval_block_box(body, scope);
                            }
                        }
                        if (x.else_body) {
                            Environment* scope = m_heap.make_env(env);
                            return eval_block_box(x.else_body, scope);
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, NumericFor>) {
                        // for v = init, limit [, step] do body end
                        LuaValue initv = eval_scalar(*x.init, env);
                        LuaValue limv  = eval_scalar(*x.limit, env);
                        LuaValue stepv = x.step ? eval_scalar(**x.step, env)
                                                : LuaValue::integer(1);
                        double init, lim, step;
                        if (!for_num(initv, init) || !for_num(limv, lim) ||
                            !for_num(stepv, step))
                            throw LuaError("'for' initial value must be a number",
                                           node.start());
                        if (step == 0.0)
                            throw LuaError("'for' step is zero", node.start());
                        // Keep int subtype when all three are ints (Lua 5.4).
                        bool int_loop = initv.is_int() && limv.is_int() &&
                                        stepv.is_int();
                        long long ii = static_cast<long long>(init);
                        long long il = static_cast<long long>(lim);
                        long long is = static_cast<long long>(step);
                        while (true) {
                            if (int_loop) {
                                if (is > 0 ? ii > il : ii < il) break;
                            } else {
                                if (step > 0 ? init > lim : init < lim) break;
                            }
                            Environment* scope = m_heap.make_env(env);
                            scope->vars[x.var] = int_loop
                                ? LuaValue::integer(ii)
                                : LuaValue::flt(init);
                            Control c = eval_block_box(x.body, scope);
                            if (c.flow == Flow::Break)    break;
                            if (c.flow == Flow::Return)   return c;
                            if (int_loop) ii += is;
                            else           init += step;
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, GenericFor>) {
                        // for v1,...,vN in explist do body end
                        // The explist yields 1..3 iterator-control values:
                        //   f, s, var  (invarient state + control variable)
                        ValueVec it = eval_explist(x.exprs, env);
                        while (it.size() < 3) it.push_back(LuaValue::nil());
                        LuaValue f = it[0], s = it[1], var = it[2];
                        if (!f.is_callable())
                            throw LuaError("attempt to call a " +
                                           std::string(type_name(f)) +
                                           " value (for iterator)", node.start());
                        while (true) {
                            ValueVec args{s, var};
                            ValueVec out = call_value(f, std::move(args), node.start());
                            LuaValue next = out.empty() ? LuaValue::nil() : out[0];
                            if (next.is_nil()) break;
                            var = next;
                            Environment* scope = m_heap.make_env(env);
                            // Bind loop vars: first gets `next`, rest get the
                            // additional returns (multires on the iterator call).
                            for (std::size_t i = 0; i < x.names.size(); ++i) {
                                scope->vars[x.names[i]] =
                                    i < out.size() ? out[i] : LuaValue::nil();
                            }
                            Control c = eval_block_box(x.body, scope);
                            if (c.flow == Flow::Break)    break;
                            if (c.flow == Flow::Return)   return c;
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, Break>) {
                        Control c; c.flow = Flow::Break; return c;
                    }
                    else if constexpr (std::is_same_v<T, LocalFunction>) {
                        // local function Name funcbody. Lua desugars this to
                        //   local f; f = function() body end
                        // so the body can reference f recursively. We bind f
                        // first (to nil), then create the closure in the SAME
                        // scope so it captures the binding.
                        env->vars[x.name] = LuaValue::nil();
                        bool va = false;
                        for (const auto& p : x.body.params)
                            if (p.kind == Param::Kind::Vararg) va = true;
                        LuaValue clo = LuaValue::closure(
                            m_heap.make_closure(&x.body, env, va));
                        env->vars[x.name] = clo;
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, FuncStat>) {
                        // function a.b.c:d () body end. Resolve the base (a),
                        // walk dotted fields to the parent, then assign the
                        // method/closure into the parent table.
                        const FuncName& nm = x.name;
                        if (nm.fields.empty())
                            throw LuaError("malformed function statement", node.start());
                        // Build the closure first.
                        bool va = false;
                        for (const auto& p : x.body.params)
                            if (p.kind == Param::Kind::Vararg) va = true;
                        LuaValue clo = LuaValue::closure(
                            m_heap.make_closure(&x.body, env, va));
                        // Base name.
                        LuaValue base = lookup(env, nm.fields[0]);
                        // Intermediate table fields.
                        for (std::size_t i = 1; i + 1 < nm.fields.size(); ++i) {
                            base = index_get(base, nm.fields[i], node.start());
                        }
                        if (nm.fields.size() == 1 && !nm.method) {
                            // function f() ... end  -> global/local f
                            assign(env, nm.fields[0], clo, node.start());
                        }
                        else if (nm.method) {
                            // parent:m  ->  store 'm' (the method) into parent,
                            // with the method's implicit self = parent.
                            index_set(base,
                                      LuaValue::str(m_heap.make_string(*nm.method)),
                                      clo, node.start());
                        }
                        else {
                            // a.b = closure
                            index_set(base,
                                      LuaValue::str(m_heap.make_string(nm.fields.back())),
                                      clo, node.start());
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, CallStat>) {
                        // A function call used as a statement: discard results.
                        if (holds<Call>(*x.call))
                            (void)eval_call(get<Call>(*x.call), env);
                        else if (holds<MethodCall>(*x.call)) {
                            const MethodCall& mc = get<MethodCall>(*x.call);
                            LuaValue self = eval_scalar(*mc.obj, env);
                            LuaValue func = index_get(self, mc.method, node.start());
                            ValueVec args;
                            args.push_back(self);
                            for (LuaValue& a : eval_explist(mc.args, env))
                                args.push_back(std::move(a));
                            (void)call_value(func, std::move(args), node.start());
                        }
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, Goto> ||
                                       std::is_same_v<T, Label>) {
                        // Deferred per the M2.0 plan (clear error, not silent).
                        throw LuaError("goto/labels are not yet supported",
                                       node.start());
                    }
                    else if constexpr (std::is_same_v<T, Nil> ||
                                       std::is_same_v<T, Skip>) {
                        return {};   // structural placeholders
                    }
                    else {
                        // An expression appearing in statement position is a
                        // (currently unsupported) call statement; the typed
                        // CallStat branch above handles it once implemented.
                        throw LuaError("unexpected node in statement position",
                                       node.start());
                    }
                },
                node.v);
        }

        // -------------------------------------------------------------------
        // Expression evaluation (multires-aware)
        // -------------------------------------------------------------------
        ValueVec Evaluator::eval_expr(const AstNode& node, Environment* env)
        {
            // Multires: a Call/MethodCall/Vararg in tail position yields all its
            // values; everything else yields exactly one. Paren ALWAYS truncates
            // to one (§3.4.12) — that's why the AST preserves it.
            if (holds<Call>(node)) {
                return eval_call(get<Call>(node), env);
            }
            if (holds<MethodCall>(node)) {
                // obj:method(args) -> obj.method(obj, args), multires result.
                const MethodCall& mc = get<MethodCall>(node);
                LuaValue self = eval_scalar(*mc.obj, env);
                LuaValue func = index_get(self, mc.method, node.start());
                ValueVec args;
                args.push_back(self);
                for (LuaValue& a : eval_explist(mc.args, env))
                    args.push_back(std::move(a));
                return call_value(func, std::move(args), node.start());
            }
            if (holds<Vararg>(node)) {
                return env->varargs;   // copy: '...' expands to all varargs
            }
            return ValueVec{eval_scalar(node, env)};
        }

        LuaValue Evaluator::eval_scalar(const AstNode& node, Environment* env)
        {
            return std::visit(
                [&](const auto& x) -> LuaValue {
                    using T = std::remove_cvref_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, Nil>) {
                        return LuaValue::nil();
                    }
                    else if constexpr (std::is_same_v<T, Skip>) {
                        return LuaValue::nil();   // transparent
                    }
                    else if constexpr (std::is_same_v<T, True>) {
                        return LuaValue::boolean(true);
                    }
                    else if constexpr (std::is_same_v<T, False>) {
                        return LuaValue::boolean(false);
                    }
                    else if constexpr (std::is_same_v<T, IntLit>) {
                        return LuaValue::integer(x.value);
                    }
                    else if constexpr (std::is_same_v<T, FltLit>) {
                        return LuaValue::flt(x.value);
                    }
                    else if constexpr (std::is_same_v<T, StrLit>) {
                        return LuaValue::str(m_heap.make_string(x.value));
                    }
                    else if constexpr (std::is_same_v<T, Name>) {
                        return lookup(env, x.name);
                    }
                    else if constexpr (std::is_same_v<T, BinOp>) {
                        LuaValue lhs = eval_scalar(*x.lhs, env);
                        // and/or short-circuit: evaluate rhs only if needed.
                        if (x.op == BinOpKind::And) {
                            if (!lhs.truthy()) return lhs;   // falsy -> return it
                            return eval_scalar(*x.rhs, env);
                        }
                        if (x.op == BinOpKind::Or) {
                            if (lhs.truthy()) return lhs;     // truthy -> return it
                            return eval_scalar(*x.rhs, env);
                        }
                        LuaValue rhs = eval_scalar(*x.rhs, env);
                        std::size_t off = x.start;
                        switch (x.op) {
                        case BinOpKind::Add:     return arith_add(lhs, rhs, off);
                        case BinOpKind::Sub:     return arith_sub(lhs, rhs, off);
                        case BinOpKind::Mul:     return arith_mul(lhs, rhs, off);
                        case BinOpKind::Div:     return arith_div(lhs, rhs, off);
                        case BinOpKind::FloorDiv:return arith_idiv(lhs, rhs, off);
                        case BinOpKind::Mod:     return arith_mod(lhs, rhs, off);
                        case BinOpKind::Pow:     return arith_pow(lhs, rhs, off);
                        case BinOpKind::BAnd:    return arith_band(lhs, rhs, off);
                        case BinOpKind::BOr:     return arith_bor (lhs, rhs, off);
                        case BinOpKind::BXor:    return arith_bxor(lhs, rhs, off);
                        case BinOpKind::Shl:     return arith_shl (lhs, rhs, off);
                        case BinOpKind::Shr:     return arith_shr (lhs, rhs, off);
                        case BinOpKind::Concat: {
                            if (!concat_ok(lhs))
                                throw LuaError(
                                    std::string("attempt to concatenate a ") +
                                    type_name(lhs) + " value", off);
                            if (!concat_ok(rhs))
                                throw LuaError(
                                    std::string("attempt to concatenate a ") +
                                    type_name(rhs) + " value", off);
                            std::string s = concat_part(lhs) + concat_part(rhs);
                            return LuaValue::str(m_heap.make_string(std::move(s)));
                        }
                        case BinOpKind::Eq: return LuaValue::boolean(raw_equal(lhs, rhs));
                        case BinOpKind::Ne: return LuaValue::boolean(!raw_equal(lhs, rhs));
                        case BinOpKind::Lt: return LuaValue::boolean(raw_cmp(lhs, rhs, off) < 0);
                        case BinOpKind::Le: return LuaValue::boolean(raw_cmp(lhs, rhs, off) <= 0);
                        case BinOpKind::Gt: return LuaValue::boolean(raw_cmp(lhs, rhs, off) > 0);
                        case BinOpKind::Ge: return LuaValue::boolean(raw_cmp(lhs, rhs, off) >= 0);
                        }
                        throw LuaError("unknown binary operator", off);
                    }
                    else if constexpr (std::is_same_v<T, UnOp>) {
                        LuaValue operand = eval_scalar(*x.operand, env);
                        std::size_t off = x.start;
                        switch (x.op) {
                        case UnOpKind::Neg: return arith_unm(operand, off);
                        case UnOpKind::Not: return arith_not(operand);
                        case UnOpKind::Len: return arith_len(operand, off);
                        case UnOpKind::BNot: return arith_bnot(operand, off);
                        }
                        throw LuaError("unknown unary operator", off);
                    }
                    else if constexpr (std::is_same_v<T, Paren>) {
                        // Parenthesized expression. As a SCALAR it is just its
                        // inner value (the multires-truncation meaning of Paren
                        // only matters in an explist/arglist context).
                        return eval_scalar(*x.exp, env);
                    }
                    else if constexpr (std::is_same_v<T, Call>) {
                        ValueVec r = eval_call(x, env);
                        return r.empty() ? LuaValue::nil() : r.front();
                    }
                    else if constexpr (std::is_same_v<T, MethodCall>) {
                        // obj:method(args) desugars to obj.method(obj, args).
                        LuaValue self = eval_scalar(*x.obj, env);
                        LuaValue func = index_get(self, x.method, node.start());
                        ValueVec args;
                        args.push_back(self);
                        for (LuaValue& a : eval_explist(x.args, env))
                            args.push_back(std::move(a));
                        ValueVec r = call_value(func, std::move(args), node.start());
                        return r.empty() ? LuaValue::nil() : r.front();
                    }
                    else if constexpr (std::is_same_v<T, FuncDef>) {
                        // function funcbody: a closure capturing the current env.
                        bool va = false;
                        for (const auto& p : x.body.params)
                            if (p.kind == Param::Kind::Vararg) va = true;
                        return LuaValue::closure(
                            m_heap.make_closure(&x.body, env, va));
                    }
                    else if constexpr (std::is_same_v<T, Vararg>) {
                        // As a scalar, '...' is its first value (or nil).
                        return env->varargs.empty() ? LuaValue::nil()
                                                    : env->varargs.front();
                    }
                    else if constexpr (std::is_same_v<T, Index>) {
                        // prefixexp [ exp ]
                        LuaValue obj = eval_scalar(*x.obj, env);
                        LuaValue key = eval_scalar(*x.key, env);
                        return index_get(obj, key, node.start());
                    }
                    else if constexpr (std::is_same_v<T, Field>) {
                        // prefixexp . Name
                        LuaValue obj = eval_scalar(*x.obj, env);
                        return index_get(obj, x.name, node.start());
                    }
                    else if constexpr (std::is_same_v<T, TableCtor>) {
                        // { fieldlist }. Positional fields get keys 1, 2, ...;
                        // named fields use their Name; bracketed use the
                        // computed key. The LAST positional field expands its
                        // multires into consecutive integer keys.
                        Table* t = m_heap.make_table();
                        LuaValue tv = LuaValue::table(t);
                        long long idx = 1;
                        for (std::size_t fi = 0; fi < x.fields.size(); ++fi) {
                            const FieldEntry& f = x.fields[fi];
                            if (f.kind == FieldEntry::Kind::Named) {
                                LuaValue v = eval_scalar(*f.value, env);
                                LuaKey k; k.k = LuaKey::K::Str; k.s = f.name;
                                if (!v.is_nil()) t->hash[k] = v;
                            }
                            else if (f.kind == FieldEntry::Kind::Bracketed) {
                                LuaValue k0 = eval_scalar(*f.key, env);
                                LuaValue v  = eval_scalar(*f.value, env);
                                LuaKey k;
                                if (!to_key(k0, k))
                                    throw LuaError("table index is nil/NaN",
                                                   node.start());
                                if (!v.is_nil()) t->hash[k] = v;
                            }
                            else {   // Positional
                                bool is_last = (fi + 1 == x.fields.size());
                                if (is_last) {
                                    // Last positional: expand multires.
                                    ValueVec vs = eval_expr(*f.value, env);
                                    for (LuaValue& v : vs) {
                                        if (!v.is_nil()) {
                                            LuaKey k; k.k = LuaKey::K::Int;
                                            k.i = idx;
                                            t->hash[k] = std::move(v);
                                        }
                                        ++idx;
                                    }
                                }
                                else {
                                    LuaValue v = eval_scalar(*f.value, env);
                                    if (!v.is_nil()) {
                                        LuaKey k; k.k = LuaKey::K::Int;
                                        k.i = idx;
                                        t->hash[k] = v;
                                    }
                                    ++idx;
                                }
                            }
                        }
                        return tv;
                    }
                    else {
                        throw LuaError("unexpected node in expression position",
                                       node.start());
                    }
                },
                node.v);
        }

        // -------------------------------------------------------------------
        // explist evaluation (multires-aware)
        // -------------------------------------------------------------------
        ValueVec Evaluator::eval_explist(const std::vector<Box>& exprs,
                                         Environment* env)
        {
            ValueVec out;
            if (exprs.empty()) return out;
            // All but the last contribute exactly one value (scalar).
            for (std::size_t i = 0; i + 1 < exprs.size(); ++i) {
                if (!exprs[i]) continue;
                out.push_back(eval_scalar(*exprs[i], env));
            }
            // The LAST element expands via multires (call/method/vararg) or
            // contributes one scalar otherwise.
            if (exprs.back())
                for (LuaValue& v : eval_expr(*exprs.back(), env))
                    out.push_back(std::move(v));
            return out;
        }

        // -------------------------------------------------------------------
        // Target assignment (Name / Index / Field)
        // -------------------------------------------------------------------
        void Evaluator::assign_target(const AstNode& target, Environment* env,
                                      LuaValue v, std::size_t off)
        {
            if (holds<Name>(target)) {
                assign(env, get<Name>(target).name, v, off);
                return;
            }
            if (holds<Index>(target)) {
                // t[k] = v: evaluate t and k first, then store.
                const Index& idx = get<Index>(target);
                LuaValue obj = eval_scalar(*idx.obj, env);
                LuaValue key = eval_scalar(*idx.key, env);
                index_set(obj, key, v, off);
                return;
            }
            if (holds<Field>(target)) {
                // t.k = v
                const Field& fld = get<Field>(target);
                LuaValue obj = eval_scalar(*fld.obj, env);
                index_set(obj,
                          LuaValue::str(m_heap.make_string(fld.name)),
                          v, off);
                return;
            }
            throw LuaError("attempt to assign to a non-variable", off);
        }

        // -------------------------------------------------------------------
        // Calling functions (closures + builtins)
        // -------------------------------------------------------------------
        ValueVec Evaluator::call_value(const LuaValue& f, ValueVec args,
                                       std::size_t off)
        {
            if (!f.is_callable())
                throw LuaError("attempt to call a " +
                               std::string(type_name(f)) + " value", off);
            if (f.is_builtin())
                return f.as_builtin()->fn(*this, std::move(args));

            // Closure call. Depth guard prevents C-stack overflow on runaway
            // recursion (keeps ASan from segfaulting).
            if (++m_depth > 200) {
                --m_depth;
                throw LuaError("stack overflow", off);
            }
            Closure* clo = f.as_closure();
            const FuncBody& fb = *clo->body;
            // New frame env, child of the closure's CAPTURED env (lexical scope).
            Environment* frame = m_heap.make_env(clo->env);
            // Bind params: extras truncated, missing -> nil. A trailing '...'
            // collects the leftover arguments.
            std::size_t nparams = 0;
            for (std::size_t i = 0; i < fb.params.size(); ++i) {
                if (fb.params[i].kind == Param::Kind::Vararg) {
                    // Collect leftovers into the frame's varargs.
                    for (std::size_t j = i; j < args.size(); ++j)
                        frame->varargs.push_back(args[j]);
                    break;
                }
                std::string pname = fb.params[i].name;
                frame->vars[pname] = i < args.size() ? args[i] : LuaValue::nil();
                ++nparams;
            }
            (void)nparams;
            Control c = eval_block(get<Block>(*fb.body), frame);
            --m_depth;
            if (c.flow == Flow::Return)
                return std::move(c.vals);
            return {};   // fell off the end -> no values
        }

        // Evaluate a Call node: func expr + arg exprs -> result values.
        ValueVec Evaluator::eval_call(const Call& c, Environment* env)
        {
            LuaValue f = eval_scalar(*c.func, env);
            ValueVec args = eval_explist(c.args, env);
            return call_value(f, std::move(args), c.start);
        }

        // Indexing reads. Tables use LuaKey lookup (Step 6); strings error for
        // now (string indexing needs the string library, M3). A Field access
        // `t.k` is `t["k"]`.
        LuaValue Evaluator::index_get(const LuaValue& obj, const LuaValue& key,
                                      std::size_t off)
        {
            (void)off;
            if (obj.is_table()) {
                LuaKey k;
                if (!to_key(key, k)) return LuaValue::nil();   // nil/NaN key -> nil
                auto it = obj.as_table()->hash.find(k);
                return it == obj.as_table()->hash.end() ? LuaValue::nil()
                                                        : it->second;
            }
            // Strings/other types: indexing needs metatables/string lib (M3).
            return LuaValue::nil();
        }

        LuaValue Evaluator::index_get(const LuaValue& obj, std::string_view key,
                                      std::size_t off)
        {
            return index_get(obj,
                             LuaValue::str(m_heap.make_string(std::string{key})),
                             off);
        }

        // Table store. Rejects nil/NaN keys (Lua: silently dropped for nil,
        // error for NaN). Recomputes the border lazily on #.
        void Evaluator::index_set(const LuaValue& obj, const LuaValue& key,
                                  LuaValue v, std::size_t off)
        {
            (void)off;
            if (!obj.is_table())
                throw LuaError("attempt to index a " +
                               std::string(type_name(obj)) + " value", off);
            Table* t = obj.as_table();
            LuaKey k;
            if (!to_key(key, k)) {
                if (key.is_nil()) return;   // t[nil] = v is a no-op in Lua
                throw LuaError("table index is NaN", off);
            }
            if (v.is_nil()) t->hash.erase(k);
            else            t->hash[k] = v;
        }

        // -------------------------------------------------------------------
        // Name lookup + assignment
        // -------------------------------------------------------------------
        LuaValue Evaluator::lookup(Environment* env, std::string_view name)
        {
            for (Environment* e = env; e; e = e->parent) {
                auto it = e->vars.find(std::string{name});
                if (it != e->vars.end()) return it->second;
            }
            return LuaValue::nil();   // globals default to nil
        }

        void Evaluator::assign(Environment* env, std::string_view name,
                               LuaValue v, std::size_t off)
        {
            // Walk the chain; assign to the existing binding, or fall back to
            // the global environment (Lua semantics: a name without a local
            // binding is a global).
            for (Environment* e = env; e; e = e->parent) {
                if (e->vars.find(std::string{name}) != e->vars.end()) {
                    // <const> enforcement.
                    if (e->consts.count(std::string{name}))
                        throw LuaError("attempt to assign to const variable '" +
                                       std::string{name} + "'", off);
                    e->vars[std::string{name}] = v;
                    return;
                }
            }
            m_globals->vars[std::string{name}] = v;
        }

    } // namespace lua
} // namespace ys
