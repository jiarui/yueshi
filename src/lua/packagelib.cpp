#include "lua/packagelib.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "lua/ast.h"
#include "lua/compile.h"
#include "lua/goto_check.h"

namespace ys
{
    namespace lua
    {
        namespace packagelib {

        // -------------------------------------------------------------------
        // Helpers for table manipulation.
        // -------------------------------------------------------------------

        static void set_str(Table* t, const char* k, std::string v, Heap& h)
        {
            LuaKey key; key.k = LuaKey::K::Str; key.s = k;
            t->hash[key] = LuaValue::str(h.make_string(std::move(v)));
        }

        static void set_tbl(Table* t, const char* k, Table* v)
        {
            LuaKey key; key.k = LuaKey::K::Str; key.s = k;
            t->hash[key] = LuaValue::table(v);
        }

        static void set_fn(Table* t, const char* k, BuiltinFn fn, Heap& h)
        {
            LuaKey key; key.k = LuaKey::K::Str; key.s = k;
            t->hash[key] = LuaValue::builtin(h.make_builtin(k, fn));
        }

        static Table* get_sub_table(Table* t, const char* k)
        {
            LuaKey key; key.k = LuaKey::K::Str; key.s = k;
            auto it = t->hash.find(key);
            return (it != t->hash.end() && it->second.is_table())
                       ? it->second.as_table() : nullptr;
        }

        static std::string get_str_field(Table* t, const char* k)
        {
            LuaKey key; key.k = LuaKey::K::Str; key.s = k;
            auto it = t->hash.find(key);
            return (it != t->hash.end() && it->second.is_str())
                       ? it->second.as_str()->data : "";
        }

        // Get the package table from _G.
        static Table* get_package(Evaluator& ev)
        {
            LuaKey pk; pk.k = LuaKey::K::Str; pk.s = "package";
            auto it = ev.globals().hash.find(pk);
            return (it != ev.globals().hash.end() && it->second.is_table())
                       ? it->second.as_table() : nullptr;
        }

        // -------------------------------------------------------------------
        // searchpath_impl: the core path-search algorithm.
        // -------------------------------------------------------------------
        struct SearchResult { std::string found; std::string err; };

        static SearchResult searchpath_impl(const std::string& name,
                                             const std::string& path,
                                             const std::string& sep,
                                             const std::string& rep)
        {
            // Replace sep with rep in the module name.
            std::string dotted = name;
            if (!sep.empty()) {
                for (std::size_t pos = 0;
                     (pos = dotted.find(sep, pos)) != std::string::npos; )
                    dotted.replace(pos, sep.size(), rep);
            }

            std::string err;
            std::size_t start = 0;
            while (start <= path.size()) {
                std::size_t semi = path.find(';', start);
                std::string tmpl = path.substr(
                    start, (semi == std::string::npos)
                               ? path.size() - start : semi - start);
                start = (semi == std::string::npos)
                            ? path.size() + 1 : semi + 1;
                if (tmpl.empty()) continue;

                // Replace '?' with the dotted name.
                std::string fname = tmpl;
                for (std::size_t pos = 0;
                     (pos = fname.find('?', pos)) != std::string::npos; )
                    fname.replace(pos, 1, dotted);

                // Check if file exists (readable).
                std::ifstream ifs(fname, std::ios::binary);
                if (ifs) {
                    return {fname, ""};
                }
                err += "\tno file '" + fname + "'\n";
            }
            // Trim trailing newline.
            if (!err.empty() && err.back() == '\n') err.pop_back();
            return {"", err};
        }

        // -------------------------------------------------------------------
        // searchpath builtin.
        // -------------------------------------------------------------------
        ValueVec b_searchpath(Evaluator& ev, ValueVec args)
        {
            std::string name = (args.size() >= 1 && args[0].is_str())
                                   ? args[0].as_str()->data : "";
            std::string path = (args.size() >= 2 && args[1].is_str())
                                   ? args[1].as_str()->data : "";
            std::string sep  = (args.size() >= 3 && args[2].is_str())
                                   ? args[2].as_str()->data : ".";
            std::string rep  = (args.size() >= 4 && args[3].is_str())
                                   ? args[3].as_str()->data : "/";

            auto r = searchpath_impl(name, path, sep, rep);
            if (!r.found.empty())
                return {LuaValue::str(ev.heap().make_string(r.found))};
            return {LuaValue::nil(),
                    LuaValue::str(ev.heap().make_string(r.err))};
        }

        // -------------------------------------------------------------------
        // make_chunk_closure: compile source + create a vararg closure.
        // -------------------------------------------------------------------
        static Closure* make_chunk_closure(Evaluator& ev,
                                            const std::string& source,
                                            Table* env_table)
        {
            ParseResult pr = compile_source(source);
            if (!pr) {
                std::string msg = pr.errors.empty() ? "syntax error"
                                                    : pr.errors[0];
                throw LuaError(msg, 0);
            }
            // Goto scope check.
            check_goto_scopes(*pr.ast, source);
            // Synthesize FuncBody from Chunk.
            const FuncBody* fb = ev.retain_chunk_ast(std::move(*pr.ast));
            return ev.heap().make_closure(fb, nullptr, env_table, true);
        }

        // -------------------------------------------------------------------
        // Searcher 1: preload.
        // -------------------------------------------------------------------
        ValueVec b_searcher_preload(Evaluator& ev, ValueVec args)
        {
            std::string modname = (args.size() >= 1 && args[0].is_str())
                                      ? args[0].as_str()->data : "";
            Table* pkg = get_package(ev);
            if (pkg) {
                if (Table* preload = get_sub_table(pkg, "preload")) {
                    LuaKey mk; mk.k = LuaKey::K::Str; mk.s = modname;
                    auto it = preload->hash.find(mk);
                    if (it != preload->hash.end() &&
                        (it->second.is_closure() || it->second.is_builtin())) {
                        return {it->second,
                                LuaValue::str(
                                    ev.heap().make_string(":preload:"))};
                    }
                }
            }
            return {LuaValue::str(ev.heap().make_string(
                "\tno field package.preload['" + modname + "']"))};
        }

        // -------------------------------------------------------------------
        // Searcher 2: Lua path.
        // -------------------------------------------------------------------
        ValueVec b_searcher_path(Evaluator& ev, ValueVec args)
        {
            std::string modname = (args.size() >= 1 && args[0].is_str())
                                      ? args[0].as_str()->data : "";
            Table* pkg = get_package(ev);
            if (!pkg)
                throw LuaError("package table not found", 0);

            std::string path = get_str_field(pkg, "path");
            if (path.empty())
                throw LuaError("'package.path' must be a string", 0);

            std::string dirsep = "/";
            { auto config = get_str_field(pkg, "config");
              if (!config.empty()) dirsep = config.substr(0, 1); }

            auto r = searchpath_impl(modname, path, ".", dirsep);
            if (r.found.empty())
                return {LuaValue::str(ev.heap().make_string(r.err))};

            // Load the file.
            std::ifstream is(r.found, std::ios::binary);
            if (!is)
                return {LuaValue::str(ev.heap().make_string(
                    "\tcannot open " + r.found))};
            std::string source{std::istreambuf_iterator<char>{is}, {}};

            try {
                Closure* clo = make_chunk_closure(ev, source, &ev.globals());
                return {LuaValue::closure(clo),
                        LuaValue::str(ev.heap().make_string(r.found))};
            } catch (const LuaError& e) {
                throw LuaError("error loading module '" + modname +
                               "' from file '" + r.found + "':\n" +
                               e.what(), 0);
            }
        }

        // -------------------------------------------------------------------
        // Searcher 3: C path (stub — no dlopen).
        // -------------------------------------------------------------------
        ValueVec b_searcher_cpath(Evaluator& ev, ValueVec args)
        {
            std::string modname = (args.size() >= 1 && args[0].is_str())
                                      ? args[0].as_str()->data : "";
            Table* pkg = get_package(ev);
            std::string cpath = pkg ? get_str_field(pkg, "cpath") : "";
            std::string dirsep = "/";
            if (pkg) { auto config = get_str_field(pkg, "config");
                       if (!config.empty()) dirsep = config.substr(0, 1); }

            auto r = searchpath_impl(modname, cpath, ".", dirsep);
            // Always fail — no C module loading.
            return {LuaValue::str(ev.heap().make_string(r.err))};
        }

        // -------------------------------------------------------------------
        // Searcher 4: all-in-one (stub).
        // -------------------------------------------------------------------
        ValueVec b_searcher_allinone(Evaluator& ev, ValueVec args)
        {
            std::string modname = (args.size() >= 1 && args[0].is_str())
                                      ? args[0].as_str()->data : "";
            return {LuaValue::str(ev.heap().make_string(
                "\tno C module '" + modname + "'"))};
        }

        // -------------------------------------------------------------------
        // loadlib stub.
        // -------------------------------------------------------------------
        ValueVec b_loadlib(Evaluator& ev, ValueVec /*args*/)
        {
            return {LuaValue::nil(),
                    LuaValue::str(ev.heap().make_string(
                        "dynamic libraries not enabled in this yueshi build")),
                    LuaValue::str(ev.heap().make_string("absent"))};
        }

        // -------------------------------------------------------------------
        // require builtin.
        // -------------------------------------------------------------------
        ValueVec b_require(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                throw LuaError("bad argument #1 to 'require' "
                               "(string expected)", 0);
            std::string modname = args[0].as_str()->data;

            Table* pkg = get_package(ev);
            if (!pkg)
                throw LuaError("package table not found", 0);
            Table* loaded = get_sub_table(pkg, "loaded");
            if (!loaded)
                throw LuaError("package.loaded not found", 0);

            // 1. Check cache.
            LuaKey mk; mk.k = LuaKey::K::Str; mk.s = modname;
            auto it = loaded->hash.find(mk);
            if (it != loaded->hash.end() && it->second.truthy()) {
                return {it->second};
            }

            // 2. Run searchers.
            Table* searchers = get_sub_table(pkg, "searchers");
            if (!searchers)
                throw LuaError("'package.searchers' must be a table", 0);

            std::string errormsg;
            LuaValue loader;
            LuaValue loader_data;

            for (long long i = 1; ; ++i) {
                LuaKey ik; ik.k = LuaKey::K::Int; ik.i = i;
                auto sit = searchers->hash.find(ik);
                if (sit == searchers->hash.end()) break;

                ValueVec sr = ev.call_value(sit->second,
                    {LuaValue::str(ev.heap().make_string(modname))}, 0);

                if (!sr.empty() &&
                    (sr[0].is_closure() || sr[0].is_builtin())) {
                    loader = sr[0];
                    loader_data = sr.size() >= 2 ? sr[1] : LuaValue::nil();
                    break;
                }
                // Searcher failed: collect error string.
                if (!sr.empty() && sr[0].is_str())
                    errormsg += sr[0].as_str()->data + "\n";
            }

            if (loader.is_nil()) {
                // Trim trailing newline.
                if (!errormsg.empty() && errormsg.back() == '\n')
                    errormsg.pop_back();
                throw LuaError("module '" + modname +
                               "' not found:\n" + errormsg, 0);
            }

            // 3. Call loader(modname, loader_data).
            ValueVec lr = ev.call_value(loader, {
                LuaValue::str(ev.heap().make_string(modname)),
                loader_data
            }, 0);

            // 4. Determine result.
            LuaValue result;
            if (!lr.empty() && !lr[0].is_nil()) {
                result = lr[0];
            } else {
                // Check if loader self-assigned package.loaded[modname].
                auto lit = loaded->hash.find(mk);
                if (lit != loaded->hash.end() && !lit->second.is_nil())
                    result = lit->second;
                else
                    result = LuaValue::boolean(true);
            }

            loaded->hash[mk] = result;
            return {result, loader_data};
        }

        } // namespace packagelib

        // -------------------------------------------------------------------
        // install_package_lib: build the package table + register require.
        // -------------------------------------------------------------------
        void install_package_lib(Evaluator& ev)
        {
            Heap& h = ev.heap();
            Table* pkg = h.make_table();

            // config: "/\n;\n?\n!\n-\n"
            packagelib::set_str(pkg, "config", "/\n;\n?\n!\n-\n", h);

            // path: from env or default.
            {
                const char* p = std::getenv("LUA_PATH_5_4");
                if (!p) p = std::getenv("LUA_PATH");
                std::string path = p ? p :
                    "./?.lua;./?/init.lua";
                // Replace ";;" with default.
                if (path.find(";;") != std::string::npos) {
                    std::string def =
                        "./?.lua;./?/init.lua";
                    std::string out;
                    std::size_t prev = 0, pos;
                    while ((pos = path.find(";;", prev)) !=
                           std::string::npos) {
                        out += path.substr(prev, pos - prev) + ";" + def;
                        prev = pos + 2;
                    }
                    out += path.substr(prev);
                    path = out;
                }
                packagelib::set_str(pkg, "path", path, h);
            }

            // cpath: from env or default.
            {
                const char* p = std::getenv("LUA_CPATH_5_4");
                if (!p) p = std::getenv("LUA_CPATH");
                packagelib::set_str(pkg, "cpath", p ? p : "./?.so", h);
            }

            // loaded: pre-seed with stdlib tables.
            {
                Table* loaded = h.make_table();
                auto seed = [&](const char* name) {
                    LuaKey gk; gk.k = LuaKey::K::Str; gk.s = name;
                    auto it = ev.globals().hash.find(gk);
                    if (it != ev.globals().hash.end()) {
                        LuaKey lk; lk.k = LuaKey::K::Str; lk.s = name;
                        loaded->hash[lk] = it->second;
                    }
                };
                seed("string");
                seed("math");
                seed("table");
                seed("io");
                seed("os");
                seed("utf8");
                // _G
                LuaKey ggk; ggk.k = LuaKey::K::Str; ggk.s = "_G";
                auto git = ev.globals().hash.find(ggk);
                if (git != ev.globals().hash.end()) {
                    LuaKey glk; glk.k = LuaKey::K::Str; glk.s = "_G";
                    loaded->hash[glk] = git->second;
                }
                packagelib::set_tbl(pkg, "loaded", loaded);
            }

            // preload: empty table.
            packagelib::set_tbl(pkg, "preload", h.make_table());

            // searchers: [preload, path, cpath, allinone].
            {
                Table* searchers = h.make_table();
                auto add_searcher = [&](long long idx, BuiltinFn fn) {
                    LuaKey k; k.k = LuaKey::K::Int; k.i = idx;
                    searchers->hash[k] =
                        LuaValue::builtin(h.make_builtin("searcher", fn));
                };
                add_searcher(1, packagelib::b_searcher_preload);
                add_searcher(2, packagelib::b_searcher_path);
                add_searcher(3, packagelib::b_searcher_cpath);
                add_searcher(4, packagelib::b_searcher_allinone);
                packagelib::set_tbl(pkg, "searchers", searchers);
            }

            // searchpath function.
            packagelib::set_fn(pkg, "searchpath", packagelib::b_searchpath, h);

            // loadlib: stub.
            packagelib::set_fn(pkg, "loadlib", packagelib::b_loadlib, h);

            // Register package in _G.
            { LuaKey pk; pk.k = LuaKey::K::Str; pk.s = "package";
              ev.globals().hash[pk] = LuaValue::table(pkg); }

            // Register require as a top-level global.
            { Builtin* req = h.make_builtin("require", packagelib::b_require);
              LuaKey rk; rk.k = LuaKey::K::Str; rk.s = "require";
              ev.globals().hash[rk] = LuaValue::builtin(req); }
        }

    } // namespace lua
} // namespace ys
