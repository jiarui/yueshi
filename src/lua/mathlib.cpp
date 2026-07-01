#include "lua/mathlib.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "lua/evaluator.h"
#include "lua/numops.h"
#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        namespace mathlib {

        // ===================================================================
        // Helpers
        // ===================================================================

        // Lua 5.4 math.* accepts floats anywhere an integer is expected, BUT
        // results preserve subtype: e.g. math.abs(-3) -> 3 (int),
        // math.abs(-3.0) -> 3.0 (float). Most cmath wrappers below produce
        // floats unconditionally; the int-preserving ones call this directly.

        // Throw a Lua-style "bad argument #N to 'fn' (number expected)".
        [[noreturn]] static void bad_arg(const char* fn, int idx,
                                         const char* expected = "number")
        {
            throw LuaError(std::string("bad argument #") +
                           std::to_string(idx + 1) + " to '" + fn +
                           "' (" + expected + " expected)", 0);
        }

        // Coerce an argument to a number (Lua 5.4 math.* does NOT string-coerce;
        // only actual numbers are accepted). Errors via bad_arg.
        static double num_arg(const ValueVec& args, int idx, const char* fn)
        {
            if (static_cast<std::size_t>(idx) >= args.size() ||
                !args[idx].is_number())
                bad_arg(fn, idx);
            return to_double(args[idx]);
        }

        // Like num_arg but returns the LuaValue (so the caller can preserve
        // subtype). Errors via bad_arg if not a number.
        static const LuaValue& valnum_arg(const ValueVec& args, int idx,
                                          const char* fn)
        {
            if (static_cast<std::size_t>(idx) >= args.size() ||
                !args[idx].is_number())
                bad_arg(fn, idx);
            return args[idx];
        }

        // Integer-coercion helper (used by random). Accepts ints and floats
        // whose value is exactly integral within long long range. Returns
        // false if not coercible.
        static bool int_arg_inner(const ValueVec& args, int idx, long long* out)
        {
            if (static_cast<std::size_t>(idx) >= args.size()) return false;
            const LuaValue& v = args[idx];
            if (v.is_int()) { *out = v.as_int(); return true; }
            if (v.is_flt()) {
                double f = v.as_flt();
                if (std::floor(f) == f &&
                    f >= static_cast<double>(std::numeric_limits<long long>::min()) &&
                    f <= static_cast<double>(std::numeric_limits<long long>::max())) {
                    *out = static_cast<long long>(f);
                    return true;
                }
            }
            return false;
        }

        // ===================================================================
        // Constants (set into the table at install time — not functions)
        // ===================================================================

        // ===================================================================
        // Subtype-preserving cmath wrappers
        // ===================================================================

        // abs preserves subtype: int in -> int out; float in -> float out.
        ValueVec b_abs(Evaluator&, ValueVec args)
        {
            const LuaValue& v = valnum_arg(args, 0, "abs");
            if (v.is_int()) {
                long long i = v.as_int();
                // LLONG_MIN's abs is undefined; Lua returns LLONG_MIN too
                // (two's-complement wrap). Match that.
                return {LuaValue::integer(i < 0 ? -i : i)};
            }
            return {LuaValue::flt(std::fabs(v.as_flt()))};
        }

        // ceil/floor: float -> integer; int -> int (unchanged).
        ValueVec b_ceil(Evaluator&, ValueVec args)
        {
            const LuaValue& v = valnum_arg(args, 0, "ceil");
            if (v.is_int()) return {v};
            double f = v.as_flt();
            // Lua 5.4 returns nil for out-of-range floats (no integer
            // representation). The corpus's math.lua exercises this.
            double r = std::ceil(f);
            if (r > static_cast<double>(std::numeric_limits<long long>::max()) ||
                r < static_cast<double>(std::numeric_limits<long long>::min()))
                return {LuaValue::nil()};
            return {LuaValue::integer(static_cast<long long>(r))};
        }

        ValueVec b_floor(Evaluator&, ValueVec args)
        {
            const LuaValue& v = valnum_arg(args, 0, "floor");
            if (v.is_int()) return {v};
            double f = v.as_flt();
            double r = std::floor(f);
            if (r > static_cast<double>(std::numeric_limits<long long>::max()) ||
                r < static_cast<double>(std::numeric_limits<long long>::min()))
                return {LuaValue::nil()};
            return {LuaValue::integer(static_cast<long long>(r))};
        }

        // exp/log/sqrt/sin/cos/tan/fmod/pow always produce a float.
        ValueVec b_exp(Evaluator&, ValueVec args)
        {
            return {LuaValue::flt(std::exp(num_arg(args, 0, "exp")))};
        }

        // log(x [, base]) — natural log if base omitted, else log_base(x).
        ValueVec b_log(Evaluator&, ValueVec args)
        {
            double x = num_arg(args, 0, "log");
            if (args.size() >= 2) {
                double b = num_arg(args, 1, "log");
                return {LuaValue::flt(std::log(x) / std::log(b))};
            }
            return {LuaValue::flt(std::log(x))};
        }

        ValueVec b_sqrt(Evaluator&, ValueVec args)
        {
            return {LuaValue::flt(std::sqrt(num_arg(args, 0, "sqrt")))};
        }

        ValueVec b_sin(Evaluator&, ValueVec args)
        {
            return {LuaValue::flt(std::sin(num_arg(args, 0, "sin")))};
        }

        ValueVec b_cos(Evaluator&, ValueVec args)
        {
            return {LuaValue::flt(std::cos(num_arg(args, 0, "cos")))};
        }

        ValueVec b_tan(Evaluator&, ValueVec args)
        {
            return {LuaValue::flt(std::tan(num_arg(args, 0, "tan")))};
        }

        // fmod: thin wrapper over std::fmod. Result subtype: float if any
        // operand is float; if both ints, integer math.fmod preserves subtype
        // (matches Lua's behavior of `%`'s subtle distinction).
        ValueVec b_fmod(Evaluator&, ValueVec args)
        {
            const LuaValue& a = valnum_arg(args, 0, "fmod");
            const LuaValue& b = valnum_arg(args, 1, "fmod");
            if (a.is_int() && b.is_int()) {
                long long bi = b.as_int();
                if (bi == 0) return {LuaValue::flt(std::nan(""))};
                return {LuaValue::integer(a.as_int() % bi)};
            }
            return {LuaValue::flt(std::fmod(to_double(a), to_double(b)))};
        }

        // modf: returns integral part AND fractional part as two values.
        ValueVec b_modf(Evaluator&, ValueVec args)
        {
            const LuaValue& v = valnum_arg(args, 0, "modf");
            double f = to_double(v);
            double ipart;
            double fpart = std::modf(f, &ipart);
            return {LuaValue::flt(ipart), LuaValue::flt(fpart)};
        }

        // pow(x, y): always a float (matches `^`).
        ValueVec b_pow(Evaluator&, ValueVec args)
        {
            double x = num_arg(args, 0, "pow");
            double y = num_arg(args, 1, "pow");
            return {LuaValue::flt(std::pow(x, y))};
        }

        // ===================================================================
        // min / max
        // ===================================================================

        // Both produce the SAME subtype as their chosen operand. Variadic.
        // min returns the smallest; max the largest. Errors on non-numbers.
        ValueVec b_min(Evaluator&, ValueVec args)
        {
            if (args.empty()) bad_arg("min", 0);
            LuaValue best = args[0];
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (!args[i].is_number())
                    throw LuaError(std::string("bad argument #") +
                                   std::to_string(i + 1) + " to 'min' "
                                   "(number expected)", 0);
                if (raw_cmp(args[i], best, 0) < 0)
                    best = args[i];
            }
            return {best};
        }

        ValueVec b_max(Evaluator&, ValueVec args)
        {
            if (args.empty()) bad_arg("max", 0);
            LuaValue best = args[0];
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (!args[i].is_number())
                    throw LuaError(std::string("bad argument #") +
                                   std::to_string(i + 1) + " to 'max' "
                                   "(number expected)", 0);
                if (raw_cmp(args[i], best, 0) > 0)
                    best = args[i];
            }
            return {best};
        }

        // ===================================================================
        // Integer-coercion queries
        // ===================================================================

        // tointeger(x): if x is already an int -> x; if x is a float that is
        // exactly integral -> that int; otherwise nil.
        ValueVec b_tointeger(Evaluator&, ValueVec args)
        {
            if (args.empty() || !args[0].is_number())
                return {LuaValue::nil()};
            const LuaValue& v = args[0];
            if (v.is_int()) return {v};
            double f = v.as_flt();
            if (std::floor(f) == f &&
                f >= static_cast<double>(std::numeric_limits<long long>::min()) &&
                f <= static_cast<double>(std::numeric_limits<long long>::max()))
                return {LuaValue::integer(static_cast<long long>(f))};
            return {LuaValue::nil()};
        }

        // type(x): "integer" / "float" for numbers; nil for non-numbers.
        ValueVec b_type(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_number())
                return {LuaValue::nil()};
            const LuaValue& v = args[0];
            const char* s = v.is_int() ? "integer" : "float";
            return {LuaValue::str(ev.heap().make_string(s))};
        }

        // ===================================================================
        // random / randomseed — PRNG lives on the Evaluator
        // ===================================================================

        // Reference Lua uses a small (but statistically-good) PRNG — a
        // 64-bit linear congruential / output-mix. We use std::mt19937_64
        // seeded the same way as Lua (default seed = 1), so test outputs
        // are reproducible across runs. (They will NOT match reference Lua's
        // exact sequence, but they share the seed reproducibility property.)
        //
        // State is held on the Evaluator via a side-table keyed by &ev so
        // multiple Evaluators don't share. We attach a small holder to ev.
        // Since the Evaluator is a long-lived object, we leak the holder
        // (like reference Lua's lstate); a destructor-aware version would
        // require Evaluator to know about mathlib, which it shouldn't.

        struct Rng {
            std::uint64_t s;   // raw state
            explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
            // xorshift64*: fast, statistically-good, and deterministic.
            std::uint64_t next()
            {
                s ^= s >> 12;
                s ^= s << 25;
                s ^= s >> 27;
                return s * 0x2545F4914F6CDD1DULL;
            }
            // Uniform double in [0, 1) — matches Lua's `random()` form.
            double uniform()
            {
                // 53 high bits -> double in [0,1).
                return static_cast<double>(next() >> 11) *
                       (1.0 / 9007199254740992.0);
            }
        };

        // One RNG per Evaluator (lazy-created on first use). The holder is
        // a static map; lifetime is process-wide (acceptable: the Evaluator
        // outlives run() but not the process).
        static std::mutex& rng_mutex()
        {
            static std::mutex m;
            return m;
        }
        static std::unordered_map<Evaluator*, Rng>& rng_map()
        {
            static std::unordered_map<Evaluator*, Rng> m;
            return m;
        }
        static Rng& rng_for(Evaluator* ev)
        {
            std::lock_guard<std::mutex> lk(rng_mutex());
            auto it = rng_map().find(ev);
            if (it == rng_map().end()) {
                auto [ins, _] = rng_map().emplace(ev, Rng{1});  // default seed = 1
                return ins->second;
            }
            return it->second;
        }

        // random([m [, n]]): three forms.
        //   random()    -> float in [0, 1)
        //   random(m)   -> integer in [1, m]
        //   random(m,n) -> integer in [m, n]
        ValueVec b_random(Evaluator& ev, ValueVec args)
        {
            Rng& r = rng_for(&ev);
            if (args.empty()) {
                return {LuaValue::flt(r.uniform())};
            }
            // integer forms
            long long m, n;
            if (!int_arg_inner(args, 0, &m))
                bad_arg("random", 0, "integer");
            if (args.size() >= 2) {
                if (!int_arg_inner(args, 1, &n))
                    bad_arg("random", 1, "integer");
            } else {
                n = m;
                m = 1;
            }
            if (n < m)
                throw LuaError("bad argument #2 to 'random' (interval is empty)",
                               0);
            // Range [m, n]: r.next() is uint64; mod (range+1).
            std::uint64_t range =
                static_cast<std::uint64_t>(n - m) + 1ULL;
            std::uint64_t v = r.next() % range;
            return {LuaValue::integer(m + static_cast<long long>(v))};
        }

        // randomseed([x [, y]]): set the PRNG seed. y is currently unused
        // (Lua 5.4 uses it for a 128-bit seed; we keep a single 64-bit state).
        ValueVec b_randomseed(Evaluator& ev, ValueVec args)
        {
            std::uint64_t seed = 1;
            if (!args.empty()) {
                if (args[0].is_int()) seed = static_cast<std::uint64_t>(args[0].as_int());
                else if (args[0].is_flt()) seed =
                    static_cast<std::uint64_t>(args[0].as_flt());
                else bad_arg("randomseed", 0, "number");
            }
            {
                std::lock_guard<std::mutex> lk(rng_mutex());
                auto it = rng_map().find(&ev);
                if (it == rng_map().end()) rng_map().emplace(&ev, Rng{seed});
                else                        it->second = Rng{seed};
            }
            return {};
        }

        } // namespace mathlib

        // -------------------------------------------------------------------
        // install_math_lib: create the `math` table, populate it, set
        // constants.
        // -------------------------------------------------------------------
        void install_math_lib(Evaluator& ev)
        {
            Heap& h = ev.heap();
            Table* mtab = h.make_table();

            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = h.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                mtab->hash[k] = LuaValue::builtin(b);
            };
            auto set_const = [&](const char* name, LuaValue v) {
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                mtab->hash[k] = v;
            };

            using namespace mathlib;
            // Functions.
            add("abs",        b_abs);
            add("ceil",       b_ceil);
            add("floor",      b_floor);
            add("exp",        b_exp);
            add("log",        b_log);
            add("sqrt",       b_sqrt);
            add("sin",        b_sin);
            add("cos",        b_cos);
            add("tan",        b_tan);
            add("fmod",       b_fmod);
            add("modf",       b_modf);
            add("pow",        b_pow);
            add("min",        b_min);
            add("max",        b_max);
            add("tointeger",  b_tointeger);
            add("type",       b_type);
            add("random",     b_random);
            add("randomseed", b_randomseed);

            // Constants.
            set_const("pi",
                LuaValue::flt(3.141592653589793238462643383279502884));
            set_const("huge",
                LuaValue::flt(std::numeric_limits<double>::infinity()));
            set_const("maxinteger",
                LuaValue::integer(std::numeric_limits<long long>::max()));
            set_const("mininteger",
                LuaValue::integer(std::numeric_limits<long long>::min()));

            { LuaKey gk; gk.k = LuaKey::K::Str; gk.s = "math";
              ev.globals().hash[gk] = LuaValue::table(mtab); }
        }

    } // namespace lua
} // namespace ys
