#include "lua/evaluator.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <ostream>
#include <utility>
#include <variant>

#include "lua/ast.h"
#include "lua/compile.h"
#include "lua/numops.h"
#include "lua/strlib.h"
#include "lua/tablib.h"
#include "lua/mathlib.h"

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
            m_G = m_heap.make_table();
            // _G._G = _G (self-reference, canonical Lua identity).
            {
                LuaKey gk; gk.k = LuaKey::K::Str; gk.s = "_G";
                m_G->hash[gk] = LuaValue::table(m_G);
            }
            // _VERSION
            {
                LuaKey vk; vk.k = LuaKey::K::Str; vk.s = "_VERSION";
                m_G->hash[vk] = LuaValue::str(m_heap.make_string("Lua 5.4"));
            }
            install_builtins();
        }

        ValueVec Evaluator::run(AstNode root)
        {
            // Clear the per-block label cache: a previous run's Block* pointers
            // would dangle (and reuse across chunks is meaningless). Each run
            // rebuilds its own caches lazily.
            m_labels.clear();

            // The chunk root is a Chunk{ Block body }.
            if (!holds<Chunk>(root))
                throw LuaError("expected a chunk at the AST root", root.start());

            Chunk& chunk = get<Chunk>(root);
            // The main chunk is a vararg function. Its `...` is empty (no
            // caller). Evaluate the body in a fresh scope whose _ENV is _G.
            // A per-run env means `local _ENV = {}` in one run doesn't leak.
            Environment* chunk_env = m_heap.make_env(nullptr);
            chunk_env->env_table = m_G;
            Control c = eval_block(chunk.body, chunk_env);
            if (c.flow == Flow::Return)
                return std::move(c.vals);
            if (c.flow == Flow::Goto)
                throw LuaError("no visible label '" + c.label +
                               "' for <goto> at chunk level", c.off);
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
                ev.out() << sep << ev.stringify(v, 0).as_str()->data;
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
            return {ev.stringify(v, 0)};
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
            // error(msg [, level]). msg may be any value; level controls
            // position prepending (Lua §2.3): level==0 -> none, level>=1 ->
            // prepend "chunk:line:" (position tracking is M4/debug-lib
            // territory; the corpus's checkerror matches substrings, so the
            // bare message suffices for M2.3).
            LuaValue v = args.empty() ? LuaValue::nil() : args[0];
            // level is unused for now — retained for API compatibility.
            // long long level = args.size() >= 2 && args[1].is_int() ? args[1].as_int() : 1;
            std::string msg = v.is_str() ? v.as_str()->data
                                         : value_to_string(v);
            LuaError err(msg, 0);
            err.obj(v);   // pcall returns the original value, not the rendering
            throw err;
        }

        ValueVec b_assert(Evaluator&, ValueVec args)
        {
            if (!args.empty() && args[0].truthy()) return args;   // pass through
            std::string msg = args.size() >= 2 ? value_to_string(args[1])
                                               : "assertion failed!";
            LuaError err(msg, 0);
            // If a message value was given, pcall should return it.
            if (args.size() >= 2) err.obj(args[1]);
            throw err;
        }

        // pcall(f, ...): call f with ...; on success return true, ...results;
        // on error return false, error-value. The error value is the original
        // error() argument (any LuaValue) if error() attached one, else the
        // error message string rendered from what().
        ValueVec b_pcall(Evaluator& ev, ValueVec args)
        {
            if (args.empty())
                throw LuaError("bad argument #1 to 'pcall' (value expected)", 0);
            LuaValue f = args[0];
            ValueVec call_args(args.begin() + 1, args.end());
            try {
                ValueVec results = ev.call_value(f, std::move(call_args), 0);
                ValueVec ret;
                ret.push_back(LuaValue::boolean(true));
                for (auto&& r : results) ret.push_back(std::move(r));
                return ret;
            }
            catch (const LuaError& e) {
                ValueVec ret;
                ret.push_back(LuaValue::boolean(false));
                ret.push_back(!e.obj().is_nil()
                    ? e.obj()
                    : LuaValue::str(ev.heap().make_string(e.what())));
                return ret;
            }
        }

        // xpcall(f, handler, ...): like pcall, but on error invokes
        // handler(err) first. Returns false, handler-result. If the handler
        // itself errors, that error propagates (natural: the second
        // call_value throws, escaping xpcall).
        ValueVec b_xpcall(Evaluator& ev, ValueVec args)
        {
            if (args.size() < 2)
                throw LuaError("bad argument to 'xpcall' (expected 2 values)", 0);
            LuaValue f = args[0];
            LuaValue handler = args[1];
            ValueVec call_args(args.begin() + 2, args.end());
            try {
                ValueVec results = ev.call_value(f, std::move(call_args), 0);
                ValueVec ret;
                ret.push_back(LuaValue::boolean(true));
                for (auto&& r : results) ret.push_back(std::move(r));
                return ret;
            }
            catch (const LuaError& e) {
                LuaValue err_val = !e.obj().is_nil()
                    ? e.obj()
                    : LuaValue::str(ev.heap().make_string(e.what()));
                ValueVec h_results = ev.call_value(
                    handler, {err_val}, 0);
                ValueVec ret;
                ret.push_back(LuaValue::boolean(false));
                for (auto&& r : h_results) ret.push_back(std::move(r));
                return ret;
            }
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

        // setmetatable(t, mt): attach a metatable to t. t must be a table; mt
        // must be a table or nil (nil clears it). Returns t. (Real Lua errors
        // if t has a __metatable field; that is deferred — we return raw.)
        ValueVec b_setmetatable(Evaluator&, ValueVec args)
        {
            LuaValue t = args.size() >= 1 ? args[0] : LuaValue::nil();
            if (!t.is_table())
                throw LuaError("bad argument #1 to 'setmetatable' "
                               "(table expected)", 0);
            LuaValue mt = args.size() >= 2 ? args[1] : LuaValue::nil();
            if (!(mt.is_table() || mt.is_nil()))
                throw LuaError("bad argument #2 to 'setmetatable' "
                               "(nil or table expected)", 0);
            t.as_table()->metatable = mt.is_nil() ? nullptr : mt.as_table();
            return {t};
        }

        // getmetatable(t): returns t's metatable, or nil if it has none.
        ValueVec b_getmetatable(Evaluator&, ValueVec args)
        {
            LuaValue t = args.size() >= 1 ? args[0] : LuaValue::nil();
            if (!t.is_table()) return {LuaValue::nil()};
            Table* mt = t.as_table()->metatable;
            return {mt ? LuaValue::table(mt) : LuaValue::nil()};
        }

        // load(chunk [, chunkname [, mode [, env]]])
        // Compiles a Lua source string into a callable closure. Returns
        // (closure) on success, (nil, errmsg) on parse error. `mode` accepts
        // "t" (text only), "b" (binary — not supported), "bt" (both, default).
        // `env` is the _ENV table for the loaded chunk (defaults to _G).
        ValueVec b_load(Evaluator& ev, ValueVec args)
        {
            LuaValue chunk = args.size() >= 1 ? args[0] : LuaValue::nil();
            std::string source;
            if (chunk.is_str()) {
                source = chunk.as_str()->data;
            } else {
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string(
                            "bad argument #1 to 'load' (string expected, got " +
                            std::string(type_name(chunk)) + ")"))};
            }

            // mode: "b" (binary — unsupported), "t" (text), "bt" (both).
            std::string mode = (args.size() >= 3 && args[2].is_str())
                ? args[2].as_str()->data : "bt";
            if (mode.find('t') == std::string::npos) {
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string(
                            "attempt to load a binary chunk (not available)"))};
            }

            // env: the _ENV for the loaded chunk. Default: _G.
            Table* env_table = &ev.globals();
            if (args.size() >= 4 && args[3].is_table())
                env_table = args[3].as_table();

            // Compile.
            ParseResult pr = compile_source(std::move(source));
            if (!pr) {
                std::string errmsg;
                for (auto& e : pr.errors) errmsg += e + "\n";
                if (!errmsg.empty() && errmsg.back() == '\n') errmsg.pop_back();
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string(errmsg))};
            }

            const FuncBody* fb = ev.retain_chunk_ast(std::move(*pr.ast));
            Closure* clo = ev.heap().make_closure(fb, nullptr, env_table, true);
            return {LuaValue::closure(clo)};
        }

        // dofile([filename])
        // Reads + compiles + executes a file. Returns the chunk's return
        // values (empty if none). With no filename, reads from stdin (needs
        // the io lib — M3.4). Errors are raised as Lua errors.
        ValueVec b_dofile(Evaluator& ev, ValueVec args)
        {
            std::string path;
            if (args.size() >= 1 && args[0].is_str()) {
                path = args[0].as_str()->data;
            } else {
                throw LuaError("bad argument #1 to 'dofile' (string expected)", 0);
            }

            std::ifstream is(path, std::ios::binary);
            if (!is) {
                throw LuaError("cannot open " + path +
                               ": No such file or directory", 0);
            }
            std::string source{std::istreambuf_iterator<char>{is}, {}};

            ParseResult pr = compile_source(std::move(source));
            if (!pr) {
                std::string msg = pr.errors.empty() ? "syntax error" : pr.errors[0];
                throw LuaError(msg, 0);
            }

            const FuncBody* fb = ev.retain_chunk_ast(std::move(*pr.ast));
            Closure* clo = ev.heap().make_closure(fb, nullptr, &ev.globals(), true);
            return ev.call_value(LuaValue::closure(clo), {}, 0);
        }

        } // namespace builtins

        void Evaluator::install_builtins()
        {
            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = m_heap.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                m_G->hash[k] = LuaValue::builtin(b);
            };
            add("print",    builtins::b_print);
            add("type",     builtins::b_type);
            add("tostring", builtins::b_tostring);
            add("tonumber", builtins::b_tonumber);
            add("error",    builtins::b_error);
            add("assert",   builtins::b_assert);
            add("pcall",    builtins::b_pcall);
            add("xpcall",   builtins::b_xpcall);
            add("ipairs",   builtins::b_ipairs);
            add("pairs",    builtins::b_pairs);
            add("next",     builtins::b_next);
            add("select",   builtins::b_select);
            add("rawget",   builtins::b_rawget);
            add("rawset",   builtins::b_rawset);
            add("rawequal", builtins::b_rawequal);
            add("rawlen",   builtins::b_rawlen);
            add("setmetatable", builtins::b_setmetatable);
            add("getmetatable", builtins::b_getmetatable);

            // Install the string library + per-type string metatable (M3.1).
            install_string_lib(*this);
            // M3.2-3: table + math libraries.
            install_table_lib(*this);
            install_math_lib(*this);

            // M3.5-B: load/loadstring/dofile.
            add("load",      builtins::b_load);
            add("loadstring", builtins::b_load);   // alias
            add("dofile",    builtins::b_dofile);
        }

        // Retain a parsed chunk's AST, returning a stable FuncBody* that
        // closures can reference. The Chunk's Block is moved into a synthesized
        // FuncBody (so the closure-call machinery, which expects FuncBody, can
        // consume it). The FuncBody lives in m_loaded_chunks for the Evaluator's
        // lifetime — not GC'd, but bounded by program behavior.
        const FuncBody* Evaluator::retain_chunk_ast(AstNode root)
        {
            if (!holds<Chunk>(root))
                throw LuaError("expected a chunk AST", root.start());
            Chunk& chunk = get<Chunk>(root);
            // Synthesize a FuncBody wrapping the chunk's Block. The main
            // chunk is vararg, so include a Vararg param marker — this is
            // what call_value checks to collect leftover args into `...`.
            FuncBody fb;
            fb.params.push_back({Param::Kind::Vararg, {}, 0, 0});
            fb.body = Box{AstNode{std::move(chunk.body)}};
            fb.start = chunk.start;
            fb.end = chunk.end;
            // Move into the retention vector.
            m_loaded_chunks.push_back(AstNode{std::move(fb)});
            return &get<FuncBody>(m_loaded_chunks.back());
        }

        // -------------------------------------------------------------------
        // Block + statement evaluation
        // -------------------------------------------------------------------
        // Index-driven so goto can resume at a matching Label. The label cache
        // is memoized in m_labels: each Block is scanned exactly once per run
        // (re-entered loop/function bodies reuse the cached map). Goto whose
        // label lives in THIS block resumes here; otherwise it propagates to an
        // enclosing block (which may catch it), or to call_value/run, which
        // report "no visible label".
        Control Evaluator::eval_block(const Block& blk, Environment* env)
        {
            LabelCache& labels = m_labels[&blk];
            if (!labels.built) {
                for (std::size_t i = 0; i < blk.stmts.size(); ++i) {
                    const Box* s = &blk.stmts[i];
                    if (*s && holds<Label>(**s))
                        labels.m[get<Label>(**s).name] = i;   // last-wins
                }
                labels.built = true;
            }
            for (std::size_t i = 0; i < blk.stmts.size(); ++i) {
                if (!blk.stmts[i]) continue;
                Control c = eval_stat(*blk.stmts[i], env);
                if (c.flow == Flow::Normal) continue;
                if (c.flow == Flow::Goto) {
                    auto it = labels.m.find(c.label);
                    if (it != labels.m.end()) {
                        i = it->second;        // land at the Label (a no-op);
                        continue;              // ++i moves past it
                    }
                    // Not in this block: propagate to the enclosing scope.
                }
                return c;   // Break/Return/unresolved-Goto propagates out
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
                            // _ENV rebind: `local _ENV = tbl` changes the
                            // scope's globals table to tbl.
                            if (x.names[i] == "_ENV" && v.is_table())
                                env->env_table = v.as_table();
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
                            if (c.flow == Flow::Goto)     return c;  // target outside loop
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
                            if (c.flow == Flow::Goto)     return c;  // target outside loop
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
                            if (c.flow == Flow::Goto)     return c;  // target outside loop
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
                            if (c.flow == Flow::Goto)     return c;  // target outside loop
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
                            m_heap.make_closure(&x.body, env, env->env_table, va));
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
                            m_heap.make_closure(&x.body, env, env->env_table, va));
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
                    else if constexpr (std::is_same_v<T, Goto>) {
                        // Emit a Goto control signal; eval_block resolves it
                        // against the per-block label cache, or propagates it
                        // outward. A goto that escapes its function/chunk is
                        // reported as "no visible label" by call_value/run.
                        Control c;
                        c.flow = Flow::Goto;
                        c.label = x.label;
                        c.off = node.start();
                        return c;
                    }
                    else if constexpr (std::is_same_v<T, Label>) {
                        // ::name:: is a no-op as a statement; eval_block's label
                        // cache is what makes it a jump target.
                        return {};
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
                        using K = BinOpKind;
                        switch (x.op) {
                        // --- arithmetic: raw when both numbers, else metamethod ---
                        case K::Add: case K::Sub: case K::Mul:
                        case K::Div: case K::FloorDiv:
                        case K::Mod: case K::Pow: {
                            if (both_numbers(lhs, rhs)) {
                                switch (x.op) {
                                case K::Add:     return arith_add(lhs, rhs, off);
                                case K::Sub:     return arith_sub(lhs, rhs, off);
                                case K::Mul:     return arith_mul(lhs, rhs, off);
                                case K::Div:     return arith_div(lhs, rhs, off);
                                case K::FloorDiv:return arith_idiv(lhs, rhs, off);
                                case K::Mod:     return arith_mod(lhs, rhs, off);
                                case K::Pow:     return arith_pow(lhs, rhs, off);
                                default: break;
                                }
                            }
                            const char* ev =
                                x.op==K::Add ? "__add" :
                                x.op==K::Sub ? "__sub" :
                                x.op==K::Mul ? "__mul" :
                                x.op==K::Div ? "__div" :
                                x.op==K::FloorDiv ? "__idiv" :
                                x.op==K::Mod  ? "__mod" : "__pow";
                            LuaValue mm = getmetamethod_bin(lhs, rhs, ev);
                            if (!mm.is_nil())
                                return call_metamethod(mm, {lhs, rhs}, off);
                            throw LuaError("attempt to perform arithmetic on a " +
                                           std::string(type_name(
                                               lhs.is_number() ? rhs : lhs)) +
                                           " value", off);
                        }
                        // --- bitwise: raw when both ints, else metamethod ---
                        case K::BAnd: case K::BOr: case K::BXor:
                        case K::Shl: case K::Shr: {
                            if (both_ints(lhs, rhs)) {
                                switch (x.op) {
                                case K::BAnd: return arith_band(lhs, rhs, off);
                                case K::BOr:  return arith_bor (lhs, rhs, off);
                                case K::BXor: return arith_bxor(lhs, rhs, off);
                                case K::Shl:  return arith_shl (lhs, rhs, off);
                                case K::Shr:  return arith_shr (lhs, rhs, off);
                                default: break;
                                }
                            }
                            const char* ev =
                                x.op==K::BAnd ? "__band" :
                                x.op==K::BOr  ? "__bor"  :
                                x.op==K::BXor ? "__bxor" :
                                x.op==K::Shl  ? "__shl"  : "__shr";
                            LuaValue mm = getmetamethod_bin(lhs, rhs, ev);
                            if (!mm.is_nil())
                                return call_metamethod(mm, {lhs, rhs}, off);
                            throw LuaError("attempt to perform bitwise operation "
                                           "on a " +
                                           std::string(type_name(
                                               lhs.is_int() ? rhs : lhs)) +
                                           " value", off);
                        }
                        // --- concatenation ---
                        case K::Concat: {
                            if (concat_ok(lhs) && concat_ok(rhs)) {
                                std::string s = concat_part(lhs) +
                                                concat_part(rhs);
                                return LuaValue::str(
                                    m_heap.make_string(std::move(s)));
                            }
                            LuaValue mm = getmetamethod_bin(lhs, rhs, "__concat");
                            if (!mm.is_nil())
                                return call_metamethod(mm, {lhs, rhs}, off);
                            const LuaValue& bad = !concat_ok(lhs) ? lhs : rhs;
                            throw LuaError(
                                std::string("attempt to concatenate a ") +
                                type_name(bad) + " value", off);
                        }
                        // --- equality (a>b derived as b<a, etc.) ---
                        case K::Eq: return LuaValue::boolean(eq_meta(lhs, rhs, off));
                        case K::Ne: return LuaValue::boolean(!eq_meta(lhs, rhs, off));
                        case K::Lt: return LuaValue::boolean(lt_meta(lhs, rhs, off));
                        case K::Le: return LuaValue::boolean(le_meta(lhs, rhs, off));
                        case K::Gt: return LuaValue::boolean(lt_meta(rhs, lhs, off));
                        case K::Ge: return LuaValue::boolean(le_meta(rhs, lhs, off));
                        }
                        throw LuaError("unknown binary operator", off);
                    }
                    else if constexpr (std::is_same_v<T, UnOp>) {
                        LuaValue operand = eval_scalar(*x.operand, env);
                        std::size_t off = x.start;
                        using U = UnOpKind;
                        switch (x.op) {
                        case U::Neg: {
                            if (operand.is_number())
                                return arith_unm(operand, off);
                            LuaValue mm = getmetamethod(operand, "__unm");
                            if (!mm.is_nil())
                                return call_metamethod(mm, {operand, operand}, off);
                            throw LuaError("attempt to perform arithmetic on a " +
                                           std::string(type_name(operand)) +
                                           " value", off);
                        }
                        case U::BNot: {
                            if (operand.is_int())
                                return arith_bnot(operand, off);
                            LuaValue mm = getmetamethod(operand, "__bnot");
                            if (!mm.is_nil())
                                return call_metamethod(mm, {operand, operand}, off);
                            throw LuaError("attempt to perform bitwise operation "
                                           "on a " +
                                           std::string(type_name(operand)) +
                                           " value", off);
                        }
                        case U::Len: {
                            // Strings always use raw length (no metatable in
                            // M2.1). Tables: __len wins if present, else raw
                            // border. Anything else needs __len or errors.
                            if (operand.is_str())
                                return arith_len(operand, off);
                            LuaValue mm = getmetamethod(operand, "__len");
                            if (!mm.is_nil())
                                return call_metamethod(mm, {operand}, off);
                            if (operand.is_table())
                                return arith_len(operand, off);
                            throw LuaError("attempt to get length of a " +
                                           std::string(type_name(operand)) +
                                           " value", off);
                        }
                        case U::Not: return arith_not(operand);
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
                            m_heap.make_closure(&x.body, env, env->env_table, va));
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
            if (!f.is_callable()) {
                // __call metamethod: Lua calls __call(f, ...) — the original
                // (non-callable) value becomes the first argument.
                LuaValue mm = getmetamethod(f, "__call");
                if (!mm.is_callable())
                    throw LuaError("attempt to call a " +
                                   std::string(type_name(f)) + " value", off);
                args.insert(args.begin(), f);
                return call_value(mm, std::move(args), off);
            }
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
            // The frame inherits the closure's captured _ENV (the globals table
            // at closure-creation time), NOT clo->env->env_table (which may have
            // been rebound since).
            frame->env_table = clo->env_table;
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
            if (c.flow == Flow::Goto)
                throw LuaError("no visible label '" + c.label +
                               "' for <goto>", c.off);
            return {};   // fell off the end -> no values
        }

        // Evaluate a Call node: func expr + arg exprs -> result values.
        ValueVec Evaluator::eval_call(const Call& c, Environment* env)
        {
            LuaValue f = eval_scalar(*c.func, env);
            ValueVec args = eval_explist(c.args, env);
            return call_value(f, std::move(args), c.start);
        }

        // -------------------------------------------------------------------
        // Metatable support (M2.1)
        // -------------------------------------------------------------------
        Table* Evaluator::metatable_of(const LuaValue& v) noexcept
        {
            if (v.is_table())   return v.as_table()->metatable;
            if (v.is_closure()) return v.as_closure()->metatable;
            if (v.is_str())     return m_string_mt;   // per-type (M3.1)
            return nullptr;
        }

        LuaValue Evaluator::getmetamethod(const LuaValue& v, std::string_view ev)
        {
            Table* mt = metatable_of(v);
            if (!mt) return LuaValue::nil();
            // Build a temporary string-key LuaKey (no GC allocation). All
            // metamethod names are short, so SSO keeps this on the stack.
            LuaKey k;
            k.k = LuaKey::K::Str;
            k.s = std::string{ev};
            auto it = mt->hash.find(k);
            return it == mt->hash.end() ? LuaValue::nil() : it->second;
        }

        LuaValue Evaluator::getmetamethod_bin(const LuaValue& a, const LuaValue& b,
                                              std::string_view ev)
        {
            // Lua's rule: the left operand's metamethod takes precedence; the
            // right's is consulted only if the left has none for this event.
            LuaValue mm = getmetamethod(a, ev);
            if (!mm.is_nil()) return mm;
            return getmetamethod(b, ev);
        }

        LuaValue Evaluator::call_metamethod(const LuaValue& mm, ValueVec args,
                                            std::size_t off)
        {
            ValueVec r = call_value(mm, std::move(args), off);
            return r.empty() ? LuaValue::nil() : r.front();
        }

        LuaValue Evaluator::stringify(const LuaValue& v, std::size_t off)
        {
            // __tostring metamethod: must return a string (Lua raises otherwise).
            LuaValue mm = getmetamethod(v, "__tostring");
            if (!mm.is_nil()) {
                LuaValue r = call_metamethod(mm, {v}, off);
                if (!r.is_str())
                    throw LuaError("'__tostring' must return a string", off);
                return r;
            }
            return LuaValue::str(m_heap.make_string(value_to_string(v)));
        }

        // Equality: raw equality covers numbers (cross-subtype), strings, nil,
        // bool, and identical objects. __eq fires only for distinct tables or
        // distinct closures that are not raw-equal (Lua §3.4.4).
        bool Evaluator::eq_meta(const LuaValue& a, const LuaValue& b,
                                std::size_t off)
        {
            if (raw_equal(a, b)) return true;
            const bool obj_pair =
                (a.is_table() && b.is_table()) ||
                (a.is_closure() && b.is_closure());
            if (!obj_pair) return false;
            LuaValue mm = getmetamethod_bin(a, b, "__eq");
            if (mm.is_nil()) return false;
            return call_metamethod(mm, {a, b}, off).truthy();
        }

        bool Evaluator::lt_meta(const LuaValue& a, const LuaValue& b,
                                std::size_t off)
        {
            if ((a.is_number() && b.is_number()) ||
                (a.is_str() && b.is_str()))
                return raw_cmp(a, b, off) < 0;
            LuaValue mm = getmetamethod_bin(a, b, "__lt");
            if (!mm.is_nil())
                return call_metamethod(mm, {a, b}, off).truthy();
            throw LuaError("attempt to compare " +
                           std::string(type_name(a)) + " with " +
                           std::string(type_name(b)), off);
        }

        bool Evaluator::le_meta(const LuaValue& a, const LuaValue& b,
                                std::size_t off)
        {
            if ((a.is_number() && b.is_number()) ||
                (a.is_str() && b.is_str()))
                return raw_cmp(a, b, off) <= 0;
            // __le; Lua falls back to `not (b < a)` via __lt when __le is absent.
            LuaValue mm = getmetamethod_bin(a, b, "__le");
            if (!mm.is_nil())
                return call_metamethod(mm, {a, b}, off).truthy();
            LuaValue mmlt = getmetamethod_bin(a, b, "__lt");
            if (!mmlt.is_nil())
                return !call_metamethod(mmlt, {b, a}, off).truthy();
            throw LuaError("attempt to compare " +
                           std::string(type_name(a)) + " with " +
                           std::string(type_name(b)), off);
        }

        // Indexing reads. Tables use LuaKey lookup; a read miss consults
        // __index (M2.1). A Field access `t.k` is `t["k"]`.
        LuaValue Evaluator::index_get(const LuaValue& obj, const LuaValue& key,
                                      std::size_t off)
        {
            // Non-table obj: only a __index metamethod can make this succeed
            // (strings would use the string metatable in M3; for now they have
            // none, so indexing them raises).
            if (!obj.is_table()) {
                LuaValue ev = getmetamethod(obj, "__index");
                if (ev.is_nil())
                    throw LuaError("attempt to index a " +
                                   std::string(type_name(obj)) + " value", off);
                if (ev.is_table()) return index_get(ev, key, off);
                return call_metamethod(ev, {obj, key}, off);
            }
            // obj is a table: walk the __index chain iteratively so a cyclic
            // prototype chain can't overflow the C stack. Each step: try the
            // raw slot (a non-nil value wins); on a miss, follow __index.
            LuaValue cur = obj;
            for (int depth = 0; depth < 100; ++depth) {
                if (!cur.is_table()) break;   // defensive; __index table checked
                Table* t = cur.as_table();
                LuaKey k;
                if (to_key(key, k)) {
                    auto it = t->hash.find(k);
                    if (it != t->hash.end() && !it->second.is_nil())
                        return it->second;
                }
                // Miss: consult cur's __index.
                LuaValue ev = getmetamethod(cur, "__index");
                if (ev.is_nil()) return LuaValue::nil();
                if (ev.is_table()) { cur = ev; continue; }   // walk into table
                return call_metamethod(ev, {cur, key}, off);  // function(cur, key)
            }
            throw LuaError("'__index' chain too long; possible loop", off);
        }

        LuaValue Evaluator::index_get(const LuaValue& obj, std::string_view key,
                                      std::size_t off)
        {
            return index_get(obj,
                             LuaValue::str(m_heap.make_string(std::string{key})),
                             off);
        }

        // Table store. A non-nil raw slot always wins (overwrite); a miss on a
        // nil-valued/absent slot consults __newindex. nil/NaN keys: nil is a
        // silent no-op, NaN errors. Iterative over the __newindex-table chain
        // to bound against cyclic metatables.
        void Evaluator::index_set(const LuaValue& obj, const LuaValue& key,
                                  LuaValue v, std::size_t off)
        {
            if (!obj.is_table()) {
                LuaValue ev = getmetamethod(obj, "__newindex");
                if (ev.is_nil())
                    throw LuaError("attempt to index a " +
                                   std::string(type_name(obj)) + " value", off);
                if (ev.is_table()) { index_set(ev, key, v, off); return; }
                call_metamethod(ev, {obj, key, v}, off);
                return;
            }
            LuaValue cur = obj;
            for (int depth = 0; depth < 100; ++depth) {
                if (!cur.is_table()) break;   // defensive
                Table* t = cur.as_table();
                LuaKey k;
                if (!to_key(key, k)) {
                    // nil key: silent no-op; NaN: error.
                    if (key.is_nil()) return;
                    throw LuaError("table index is NaN", off);
                }
                auto it = t->hash.find(k);
                if (it != t->hash.end() && !it->second.is_nil()) {
                    // Existing non-nil raw slot: raw write wins (Lua §2.4).
                    if (v.is_nil()) t->hash.erase(it);
                    else            it->second = v;
                    return;
                }
                // Slot absent/nil: consult __newindex before inserting.
                LuaValue ev = getmetamethod(cur, "__newindex");
                if (ev.is_nil()) {
                    if (!v.is_nil()) t->hash[k] = v;   // raw insert
                    return;
                }
                if (ev.is_table()) { cur = ev; continue; }   // walk into table
                call_metamethod(ev, {cur, key, v}, off);      // function(cur, key, v)
                return;
            }
            throw LuaError("'__newindex' chain too long; possible loop", off);
        }

        void Evaluator::index_set(const LuaValue& obj, std::string_view key,
                                   LuaValue v, std::size_t off)
        {
            index_set(obj, LuaValue::str(m_heap.make_string(std::string{key})),
                      v, off);
        }

        // -------------------------------------------------------------------
        // Name lookup + assignment
        // -------------------------------------------------------------------
        LuaValue Evaluator::lookup(Environment* env, std::string_view name)
        {
            // 1. Walk the local chain for an explicit binding.
            for (Environment* e = env; e; e = e->parent) {
                auto it = e->vars.find(std::string{name});
                if (it != e->vars.end()) return it->second;
            }
            // 2. _ENV is a pseudo-name: the implicit upvalue is the scope's
            //    globals table. (A `local _ENV = ...` would be found in step 1.)
            if (name == "_ENV")
                return env->env_table ? LuaValue::table(env->env_table)
                                      : LuaValue::nil();
            // 3. Global access: read via the _ENV table (honors __index).
            LuaValue env_val = env->env_table
                ? LuaValue::table(env->env_table) : LuaValue::nil();
            return index_get(env_val, name, 0);
        }

        void Evaluator::assign(Environment* env, std::string_view name,
                               LuaValue v, std::size_t off)
        {
            // _ENV is special: rebinding it changes the scope's globals table.
            // Lua 5.4 treats _ENV as an upvalue; assigning _ENV = x updates it
            // for the current scope (and the declaring scope if `local _ENV`).
            if (name == "_ENV") {
                for (Environment* e = env; e; e = e->parent) {
                    auto it = e->vars.find("_ENV");
                    if (it != e->vars.end()) {
                        if (e->consts.count("_ENV"))
                            throw LuaError("attempt to assign to const variable "
                                           "'_ENV'", off);
                        it->second = v;
                        e->env_table = v.is_table() ? v.as_table() : nullptr;
                        break;
                    }
                }
                env->env_table = v.is_table() ? v.as_table() : nullptr;
                return;
            }

            // Walk the chain; assign to the existing local, or fall back to
            // the _ENV table (a global write, honoring __newindex).
            for (Environment* e = env; e; e = e->parent) {
                auto it = e->vars.find(std::string{name});
                if (it != e->vars.end()) {
                    if (e->consts.count(std::string{name}))
                        throw LuaError("attempt to assign to const variable '" +
                                       std::string{name} + "'", off);
                    it->second = v;
                    return;
                }
            }
            LuaValue env_val = env->env_table
                ? LuaValue::table(env->env_table) : LuaValue::nil();
            index_set(env_val, name, v, off);
        }

    } // namespace lua
} // namespace ys
