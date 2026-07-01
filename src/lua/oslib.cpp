#include "lua/oslib.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <sys/wait.h>   // WEXITSTATUS / WIFSIGNALED
#include <unistd.h>

#include "lua/evaluator.h"
#include "lua/numops.h"
#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        namespace oslib {

        // -------------------------------------------------------------------
        // Helpers
        // -------------------------------------------------------------------

        [[noreturn]] static void bad_arg(const char* fn, int idx,
                                         const char* expected,
                                         const LuaValue& got)
        {
            throw LuaError(std::string("bad argument #") +
                           std::to_string(idx + 1) + " to '" + fn +
                           "' (" + expected + ", got " +
                           type_name(got) + ")", 0);
        }

        // Convert a calendar table to std::tm (best-effort).
        static std::tm table_to_tm(Evaluator& ev, const Table* t)
        {
            std::tm tm{};
            auto get_int = [&](const char* k, int def) -> int {
                LuaKey kk; kk.k = LuaKey::K::Str; kk.s = k;
                auto it = t->hash.find(kk);
                if (it == t->hash.end() || !it->second.is_number())
                    return def;
                return static_cast<int>(it->second.is_int()
                                            ? it->second.as_int()
                                            : it->second.as_flt());
            };
            tm.tm_year = get_int("year", 1970) - 1900;
            tm.tm_mon  = get_int("month", 1) - 1;
            tm.tm_mday = get_int("day", 1);
            tm.tm_hour = get_int("hour", 12);
            tm.tm_min  = get_int("min", 0);
            tm.tm_sec  = get_int("sec", 0);
            tm.tm_isdst = get_int("isdst", -1);
            (void)ev;
            return tm;
        }

        // Convert a std::tm into a Lua table on the heap.
        static Table* tm_to_table(Evaluator& ev, const std::tm& tm)
        {
            Table* t = ev.heap().make_table();
            auto set = [&](const char* k, long long v) {
                LuaKey kk; kk.k = LuaKey::K::Str; kk.s = k;
                t->hash[kk] = LuaValue::integer(v);
            };
            set("year",  tm.tm_year + 1900);
            set("month", tm.tm_mon + 1);
            set("day",   tm.tm_mday);
            set("hour",  tm.tm_hour);
            set("min",   tm.tm_min);
            set("sec",   tm.tm_sec);
            set("wday",  tm.tm_wday + 1);   // Lua: 1=Sunday
            set("yday",  tm.tm_yday + 1);
            set("isdst", tm.tm_isdst);
            return t;
        }

        // Map a Lua category string to LC_* constant.
        static int parse_category(const std::string& s)
        {
            if (s == "all")      return LC_ALL;
            if (s == "collate")  return LC_COLLATE;
            if (s == "ctype")    return LC_CTYPE;
            if (s == "monetary") return LC_MONETARY;
            if (s == "numeric")  return LC_NUMERIC;
            if (s == "time")     return LC_TIME;
            return LC_ALL;
        }

        // -------------------------------------------------------------------
        // Builtins
        // -------------------------------------------------------------------

        // os.time([t]): if t given, calendar→epoch; else current epoch.
        ValueVec b_time(Evaluator& ev, ValueVec args)
        {
            if (!args.empty() && args[0].is_table()) {
                std::tm tm = table_to_tm(ev, args[0].as_table());
                std::time_t t = std::mktime(&tm);
                if (t == static_cast<std::time_t>(-1))
                    return {LuaValue::nil()};
                return {LuaValue::integer(static_cast<long long>(t))};
            }
            std::time_t now = std::time(nullptr);
            return {LuaValue::integer(static_cast<long long>(now))};
        }

        // os.clock(): approximate CPU time used by the program, in seconds.
        // Lua returns a float; we use std::clock() / CLOCKS_PER_SEC.
        ValueVec b_clock(Evaluator&, ValueVec)
        {
            std::clock_t c = std::clock();
            double s = static_cast<double>(c) / static_cast<double>(CLOCKS_PER_SEC);
            return {LuaValue::flt(s)};
        }

        ValueVec b_difftime(Evaluator&, ValueVec args)
        {
            if (args.size() < 2 || !args[0].is_number() || !args[1].is_number())
                bad_arg("difftime", 0, "number",
                        args.empty() ? LuaValue::nil() : args[0]);
            double a = args[0].is_int() ? static_cast<double>(args[0].as_int())
                                        : args[0].as_flt();
            double b = args[1].is_int() ? static_cast<double>(args[1].as_int())
                                        : args[1].as_flt();
            return {LuaValue::flt(a - b)};
        }

        // os.date([fmt [, t]]): default "%c". fmt starts with "!" → UTC.
        // fmt starts with "*t" → table return.
        ValueVec b_date(Evaluator& ev, ValueVec args)
        {
            std::string fmt = (args.size() >= 1 && args[0].is_str())
                                  ? args[0].as_str()->data : "%c";
            std::time_t t;
            if (args.size() >= 2 && args[1].is_number()) {
                t = static_cast<std::time_t>(
                    args[1].is_int() ? args[1].as_int() : args[1].as_flt());
            } else {
                t = std::time(nullptr);
            }
            bool utc = !fmt.empty() && fmt[0] == '!';
            if (utc) fmt.erase(0, 1);
            std::tm tm = utc ? *std::gmtime(&t) : *std::localtime(&t);

            // *t format: return a table.
            if (!fmt.empty() && fmt[0] == '*') {
                if (fmt.size() >= 2 && fmt[1] == 't') {
                    return {LuaValue::table(tm_to_table(ev, tm))};
                }
            }
            char buf[256];
            std::size_t n = std::strftime(buf, sizeof(buf), fmt.c_str(), &tm);
            return {LuaValue::str(ev.heap().make_string(std::string(buf, n)))};
        }

        ValueVec b_getenv(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str()) return {LuaValue::nil()};
            const char* v = std::getenv(args[0].as_str()->data.c_str());
            if (!v) return {LuaValue::nil()};
            return {LuaValue::str(ev.heap().make_string(v))};
        }

        // os.exit([code [, close]]): close arg ignored (no to-be-closed state).
        ValueVec b_exit(Evaluator&, ValueVec args)
        {
            std::cout.flush();
            std::cerr.flush();
            int code = 0;
            if (!args.empty()) {
                if (args[0].is_bool())      code = args[0].as_bool() ? 0 : 1;
                else if (args[0].is_int())  code = static_cast<int>(args[0].as_int());
            }
            std::exit(code);
        }

        // os.execute([cmd]): run a shell command. No cmd → check if shell is
        // available (returns true). Else returns Lua 5.4's 3-value tuple.
        ValueVec b_execute(Evaluator& ev, ValueVec args)
        {
            if (args.empty()) {
                // "machine has a system command": return true.
                return {LuaValue::boolean(true)};
            }
            if (!args[0].is_str())
                bad_arg("execute", 0, "string", args[0]);
            int status = std::system(args[0].as_str()->data.c_str());
            if (status == -1) {
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string("system failed")),
                        LuaValue::integer(-1)};
            }
            if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                if (code == 0) return {LuaValue::boolean(true)};
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string("exit")),
                        LuaValue::integer(code)};
            }
            if (WIFSIGNALED(status)) {
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string("signal")),
                        LuaValue::integer(WTERMSIG(status))};
            }
            return {LuaValue::nil(),
                    LuaValue::str(ev.heap().make_string("exit")),
                    LuaValue::integer(status)};
        }

        // os.remove(name): returns (true) on success, (nil, err) on failure.
        ValueVec b_remove(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                bad_arg("remove", 0, "string",
                        args.empty() ? LuaValue::nil() : args[0]);
            if (::unlink(args[0].as_str()->data.c_str()) == -1) {
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string(
                            std::string(args[0].as_str()->data) + ": " +
                            std::strerror(errno)))};
            }
            return {LuaValue::boolean(true)};
        }

        // os.rename(old, new)
        ValueVec b_rename(Evaluator& ev, ValueVec args)
        {
            if (args.size() < 2 || !args[0].is_str() || !args[1].is_str())
                bad_arg("rename", 0, "string",
                        args.empty() ? LuaValue::nil() : args[0]);
            if (std::rename(args[0].as_str()->data.c_str(),
                            args[1].as_str()->data.c_str()) != 0) {
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string(
                            std::string(args[0].as_str()->data) + ": " +
                            std::strerror(errno)))};
            }
            return {LuaValue::boolean(true)};
        }

        // os.setlocale([locale [, category]])
        //   No arg / nil locale: query current locale for the category.
        //   String locale: set; returns the resulting locale string or nil.
        ValueVec b_setlocale(Evaluator& ev, ValueVec args)
        {
            int cat = LC_ALL;
            if (args.size() >= 2 && args[1].is_str()) {
                cat = parse_category(args[1].as_str()->data);
            }
            // Query-only path: zero args or nil locale.
            if (args.empty() || args[0].is_nil()) {
                const char* cur = std::setlocale(cat, nullptr);
                return {LuaValue::str(ev.heap().make_string(
                    cur ? cur : ""))};
            }
            if (!args[0].is_str()) return {LuaValue::nil()};
            const std::string& locale = args[0].as_str()->data;
            const char* r = std::setlocale(cat, locale.c_str());
            if (!r) return {LuaValue::nil()};
            return {LuaValue::str(ev.heap().make_string(r))};
        }

        // os.tmpname(): unique temporary filename (NOT opened). Use mkstemp +
        // immediate close + unlink so we get a unique name without the security
        // risk of tmpnam (glibc loudly warns about tmpnam; mkstemp is clean).
        ValueVec b_tmpname(Evaluator& ev, ValueVec)
        {
            char tmpl[] = "/tmp/yueshi_XXXXXX";
            int fd = ::mkstemp(tmpl);
            if (fd == -1) {
                throw LuaError("cannot generate temporary name", 0);
            }
            ::close(fd);
            ::unlink(tmpl);   // tmpname returns a NAME, not an open file
            return {LuaValue::str(ev.heap().make_string(tmpl))};
        }

        } // namespace oslib

        // -------------------------------------------------------------------
        // install_os_lib
        // -------------------------------------------------------------------
        void install_os_lib(Evaluator& ev)
        {
            Heap& h = ev.heap();
            Table* os_tab = h.make_table();
            using namespace oslib;

            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = h.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                os_tab->hash[k] = LuaValue::builtin(b);
            };

            add("time",       b_time);
            add("clock",      b_clock);
            add("difftime",   b_difftime);
            add("date",       b_date);
            add("getenv",     b_getenv);
            add("exit",       b_exit);
            add("execute",    b_execute);
            add("remove",     b_remove);
            add("rename",     b_rename);
            add("setlocale",  b_setlocale);
            add("tmpname",    b_tmpname);

            LuaKey gk; gk.k = LuaKey::K::Str; gk.s = "os";
            ev.globals().hash[gk] = LuaValue::table(os_tab);
        }

    } // namespace lua
} // namespace ys
