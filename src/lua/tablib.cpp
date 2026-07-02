#include "lua/tablib.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lua/evaluator.h"
#include "lua/numops.h"
#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        namespace tablib {

        // ===================================================================
        // Helpers
        // ===================================================================

        // Fetch a table argument; error with Lua-style message if absent/wrong.
        static Table* table_arg(const ValueVec& args, int idx, const char* fn)
        {
            if (static_cast<std::size_t>(idx) >= args.size() ||
                !args[idx].is_table()) {
                throw LuaError(std::string("bad argument #") +
                               std::to_string(idx + 1) + " to '" + fn +
                               "' (table expected)", 0);
            }
            return args[idx].as_table();
        }

        // Fetch a (coercible) integer argument; Lua 5.4 table.* accepts floats
        // whose value is exactly integral. Returns false if not coercible.
        static bool int_arg(const ValueVec& args, int idx, long long* out)
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

        // Read t[i] (integer key); returns nil if missing.
        static LuaValue arr_get(Table* t, long long i)
        {
            LuaKey k; k.k = LuaKey::K::Int; k.i = i;
            auto it = t->hash.find(k);
            return it == t->hash.end() ? LuaValue::nil() : it->second;
        }

        // Write t[i] = v (integer key); erases on nil.
        static void arr_set(Table* t, long long i, LuaValue v)
        {
            LuaKey k; k.k = LuaKey::K::Int; k.i = i;
            if (v.is_nil()) t->hash.erase(k);
            else            t->hash[k] = std::move(v);
        }

        // Get the effective length of a table, consulting __len if present
        // (matches Lua's luaL_len semantics). Used by table.insert/remove/
        // sort/concat which all respect the __len metamethod.
        static long long table_len_meta(Evaluator& ev, Table* t)
        {
            if (t->metatable) {
                LuaKey lk; lk.k = LuaKey::K::Str; lk.s = "__len";
                auto it = t->metatable->hash.find(lk);
                if (it != t->metatable->hash.end() && it->second.is_callable()) {
                    ValueVec r = ev.call_value(it->second,
                        {LuaValue::table(t)}, 0);
                    if (r.empty() || !r[0].is_number())
                        throw LuaError("object length is not an integer", 0);
                    return r[0].is_int() ? r[0].as_int()
                                         : static_cast<long long>(r[0].as_flt());
                }
            }
            return table_border(t);
        }

        // ===================================================================
        // table.insert(t, [pos,] value)
        // ===================================================================

        ValueVec b_insert(Evaluator& ev, ValueVec args)
        {
            Table* t = table_arg(args, 0, "insert");
            long long n = table_len_meta(ev, t);

            // Lua 5.4 table.insert accepts exactly 2 or 3 args.
            if (args.size() < 2 || args.size() > 3)
                throw LuaError("wrong number of arguments to 'insert'", 0);

            long long pos;
            if (args.size() == 3) {
                // 3-arg form: insert(t, pos, value). pos must be an integer.
                if (!int_arg(args, 1, &pos))
                    throw LuaError("bad argument #2 to 'insert' "
                                   "(number has no integer representation)", 0);
                if (pos < 1 || pos > n + 1)
                    throw LuaError("bad argument #2 to 'insert' (position out of "
                                   "bounds)", 0);
            } else {
                // 2-arg form: append at the end.
                pos = n + 1;
            }
            const LuaValue& v = args[args.size() == 3 ? 2 : 1];

            // Shift up: t[n+1] = t[n]; t[n] = t[n-1]; ... t[pos] = t[pos-1].
            for (long long i = n; i >= pos; --i)
                arr_set(t, i + 1, arr_get(t, i));
            arr_set(t, pos, v);
            return {};
        }

        // ===================================================================
        // table.remove(t [, pos])
        // ===================================================================

        ValueVec b_remove(Evaluator& ev, ValueVec args)
        {
            Table* t = table_arg(args, 0, "remove");
            long long n = table_len_meta(ev, t);

            long long pos;
            if (args.size() >= 2) {
                if (!int_arg(args, 1, &pos))
                    throw LuaError("bad argument #2 to 'remove' "
                                   "(number has no integer representation)", 0);
            } else {
                pos = n;   // default: remove the last element
            }
            if (n == 0) return {};   // empty array: nothing to remove
            if (pos < 1 || pos > n)
                throw LuaError("bad argument #2 to 'remove' (position out of "
                               "bounds)", 0);

            LuaValue removed = arr_get(t, pos);
            // Shift down: t[pos] = t[pos+1]; ... t[n-1] = t[n]; t[n] = nil.
            for (long long i = pos; i < n; ++i)
                arr_set(t, i, arr_get(t, i + 1));
            arr_set(t, n, LuaValue::nil());
            return {removed};
        }

        // ===================================================================
        // table.concat(list [, sep [, i [, j]]])
        // ===================================================================

        ValueVec b_concat(Evaluator& ev, ValueVec args)
        {
            Table* t = table_arg(args, 0, "concat");
            std::string sep;
            if (args.size() >= 2) {
                if (!args[1].is_str())
                    throw LuaError("bad argument #2 to 'concat' (string expected)",
                                   0);
                sep = args[1].as_str()->data;
            }
            long long n = table_border(t);
            long long i = 1, j = n;
            if (args.size() >= 3) {
                if (!int_arg(args, 2, &i))
                    throw LuaError("bad argument #3 to 'concat' "
                                   "(number has no integer representation)", 0);
            }
            if (args.size() >= 4) {
                if (!int_arg(args, 3, &j))
                    throw LuaError("bad argument #4 to 'concat' "
                                   "(number has no integer representation)", 0);
            }
            if (i < 1) i = 1;
            if (j > n) j = n;

            std::string out;
            for (long long k = i; k <= j; ++k) {
                LuaValue v = arr_get(t, k);
                std::string part;
                if (v.is_str())       part = v.as_str()->data;
                else if (v.is_int())  part = std::to_string(v.as_int());
                else if (v.is_flt())  part = number_to_string(v);
                else
                    throw LuaError(std::string("invalid value (at index ") +
                                   std::to_string(k) +
                                   ") in table for 'concat'", 0);
                if (k > i) out += sep;
                out += part;
            }
            return {LuaValue::str(ev.heap().make_string(std::move(out)))};
        }

        // ===================================================================
        // table.pack(...)
        // ===================================================================

        ValueVec b_pack(Evaluator& ev, ValueVec args)
        {
            Table* t = ev.heap().make_table();
            LuaKey nk; nk.k = LuaKey::K::Str; nk.s = "n";
            t->hash[nk] = LuaValue::integer(
                static_cast<long long>(args.size()));
            for (std::size_t i = 0; i < args.size(); ++i) {
                LuaKey k; k.k = LuaKey::K::Int;
                k.i = static_cast<long long>(i) + 1;
                t->hash[k] = args[i];
            }
            return {LuaValue::table(t)};
        }

        // ===================================================================
        // table.unpack(list [, i [, j]])
        // ===================================================================

        ValueVec b_unpack(Evaluator&, ValueVec args)
        {
            Table* t = table_arg(args, 0, "unpack");
            // i defaults to 1; nil means "use default". j defaults to the
            // table border (#t); nil means "use default". When explicit i and
            // j are given, use them verbatim (Lua allows unpacking arbitrary
            // integer-keyed ranges, including negative/huge keys).
            long long i = 1;
            long long j = table_border(t);
            if (args.size() >= 2 && !args[1].is_nil()) {
                if (!int_arg(args, 1, &i))
                    throw LuaError("bad argument #2 to 'unpack' "
                                   "(number has no integer representation)", 0);
            }
            if (args.size() >= 3 && !args[2].is_nil()) {
                if (!int_arg(args, 2, &j))
                    throw LuaError("bad argument #3 to 'unpack' "
                                   "(number has no integer representation)", 0);
            }
            // Empty range: return nothing.
            if (i > j) return {};
            // Guard against absurd ranges that would OOM. Lua 5.4 raises
            // "too many results to unpack" when the count exceeds a sane limit.
            // Use overflow-safe comparison: since i <= j, we check whether
            // the gap exceeds MAX_UNPACK. For i < 0, compute i + MAX_UNPACK
            // (safe: |i| <= LLONG_MAX, MAX_UNPACK is tiny) and compare with j.
            constexpr long long MAX_UNPACK = 8000;
            if (i >= 0) {
                if (j - i > MAX_UNPACK)
                    throw LuaError("too many results to unpack", 0);
            } else {
                // i < 0: j - i may overflow signed long long. Use i + MAX_UNPACK
                // (safe since MAX_UNPACK << |i|) as the threshold.
                if (j > i + MAX_UNPACK)
                    throw LuaError("too many results to unpack", 0);
            }
            ValueVec out;
            // Use unsigned iteration to avoid signed overflow when k == maxI.
            for (unsigned long long k = static_cast<unsigned long long>(i);
                 k <= static_cast<unsigned long long>(j);
                 ++k)
                out.push_back(arr_get(t, static_cast<long long>(k)));
            return out;
        }

        // ===================================================================
        // table.move(a1, f, e, t [, a2]) — copy a1[f..e] to a2[t..]
        // ===================================================================

        // Metamethod-aware table read: consults __index for absent keys.
        static LuaValue meta_get(Evaluator& ev, const LuaValue& tv,
                                 long long key)
        {
            Table* t = tv.as_table();
            // Raw read first.
            LuaKey k; k.k = LuaKey::K::Int; k.i = key;
            auto it = t->hash.find(k);
            if (it != t->hash.end()) return it->second;
            // Absent: consult __index metamethod (if any).
            if (t->metatable) {
                LuaKey lk; lk.k = LuaKey::K::Str; lk.s = "__index";
                auto mit = t->metatable->hash.find(lk);
                if (mit != t->metatable->hash.end()) {
                    const LuaValue& mm = mit->second;
                    if (mm.is_table()) {
                        LuaKey mk; mk.k = LuaKey::K::Int; mk.i = key;
                        auto mit2 = mm.as_table()->hash.find(mk);
                        if (mit2 != mm.as_table()->hash.end())
                            return mit2->second;
                        return LuaValue::nil();
                    }
                    if (mm.is_callable()) {
                        ValueVec r = ev.call_value(mm, {tv, LuaValue::integer(key)}, 0);
                        return r.empty() ? LuaValue::nil() : r[0];
                    }
                }
            }
            return LuaValue::nil();
        }

        // Metamethod-aware table write: consults __newindex.
        static void meta_set(Evaluator& ev, const LuaValue& tv,
                             long long key, const LuaValue& val,
                             std::size_t off)
        {
            Table* t = tv.as_table();
            // If the key is already present (raw), just update it.
            LuaKey k; k.k = LuaKey::K::Int; k.i = key;
            auto it = t->hash.find(k);
            if (it != t->hash.end()) {
                if (val.is_nil()) t->hash.erase(it);
                else              it->second = val;
                return;
            }
            // Absent: consult __newindex.
            if (t->metatable) {
                LuaKey lk; lk.k = LuaKey::K::Str; lk.s = "__newindex";
                auto mit = t->metatable->hash.find(lk);
                if (mit != t->metatable->hash.end()) {
                    const LuaValue& mm = mit->second;
                    if (mm.is_table()) {
                        // If the key exists in the __newindex table, raw-set
                        // there. Otherwise fall through to raw-set on t.
                        LuaKey mk; mk.k = LuaKey::K::Int; mk.i = key;
                        auto mit2 = mm.as_table()->hash.find(mk);
                        if (mit2 != mm.as_table()->hash.end()) {
                            if (val.is_nil()) mm.as_table()->hash.erase(mk);
                            else              mit2->second = val;
                            return;
                        }
                    } else if (mm.is_callable()) {
                        ev.call_value(mm, {tv, LuaValue::integer(key), val}, off);
                        return;
                    }
                }
            }
            // Raw set.
            if (val.is_nil()) t->hash.erase(k);
            else              t->hash[k] = val;
        }

        ValueVec b_move(Evaluator& ev, ValueVec args)
        {
            Table* a1 = table_arg(args, 0, "move");
            long long f, e, tt;
            if (!int_arg(args, 1, &f))
                throw LuaError("bad argument #2 to 'move' "
                               "(number has no integer representation)", 0);
            if (!int_arg(args, 2, &e))
                throw LuaError("bad argument #3 to 'move' "
                               "(number has no integer representation)", 0);
            if (!int_arg(args, 3, &tt))
                throw LuaError("bad argument #4 to 'move' "
                               "(number has no integer representation)", 0);
            Table* a2;
            if (args.size() >= 5) {
                if (!args[4].is_table())
                    throw LuaError("bad argument #5 to 'move' (table expected)", 0);
                a2 = args[4].as_table();
            } else {
                a2 = a1;   // default: move in place
            }

            // Validate range: "too many elements to move" and "wrap around".
            // Matches Lua 5.4's tmove: n = e - f + 1 (unsigned); if n exceeds
            // MAXSIZE → "too many"; if tt + n - 1 exceeds MAXSIZE → "wrap".
            if (e >= f) {
                using ULL = unsigned long long;
                constexpr ULL MAXSIZE =
                    static_cast<ULL>(std::numeric_limits<long long>::max());
                // Distance check first (avoids n wraparound for full-domain
                // ranges like [minI, maxI] where n would overflow to 0).
                ULL dist = static_cast<ULL>(e) - static_cast<ULL>(f);
                if (dist >= MAXSIZE)   // dist + 1 > MAXSIZE
                    throw LuaError("too many elements to move", 0);
                ULL n = dist + 1ULL;
                // dest end = tt + n - 1. Check for wrap.
                if (tt >= 0) {
                    ULL de = static_cast<ULL>(tt) + n - 1ULL;
                    if (de > MAXSIZE)
                        throw LuaError("destination wrap around", 0);
                } else {
                    // tt < 0: dest end might wrap past LLONG_MAX.
                    ULL room = MAXSIZE - static_cast<ULL>(tt);
                    if (n - 1ULL > room)
                        throw LuaError("destination wrap around", 0);
                }
            }

            if (e >= f) {
                LuaValue tv1 = LuaValue::table(a1);
                LuaValue tv2 = LuaValue::table(a2);
                // Direction: backward only when dest starts within the source
                // range (overlap requiring backward to avoid stomping unread
                // values). Otherwise forward (non-overlapping or dest-before-
                // source).
                if (tt > f && tt <= e) {
                    // Copy backward (dest overlaps source from the right).
                    for (long long k = e - f; k >= 0; --k)
                        meta_set(ev, tv2, tt + k,
                                 meta_get(ev, tv1, f + k), 0);
                } else {
                    // Copy forward.
                    for (long long k = 0; k <= e - f; ++k)
                        meta_set(ev, tv2, tt + k,
                                 meta_get(ev, tv1, f + k), 0);
                }
            }
            return {LuaValue::table(a2)};
        }

        // ===================================================================
        // table.sort(list [, comp])
        //
        // Implementation: recursive quicksort with a median-of-3 pivot and a
        // cutoff to insertion sort for small ranges (matches reference Lua's
        // shape — a sort that does N log N comparisons in the typical case and
        // degrades gracefully on adversarial input). The comparator dispatches
        // through Evaluator::call_value so a Lua comparator's metamethods are
        // honored. After sorting, a final pass validates totalness: if the
        // comparator ever returns true for BOTH (a,b) and (b,a) for some pair,
        // raise "invalid order function for sorting".
        // ===================================================================

        struct SortCtx {
            Evaluator& ev;
            LuaValue  comp;   // nil => use raw `<` (numbers/strings only)
            std::size_t off{0};
        };

        // less(a, b): returns true if a should come before b.
        static bool sort_less(SortCtx& sc, const LuaValue& a, const LuaValue& b)
        {
            if (sc.comp.is_nil()) {
                // No comparator: use the `<` operator WITH metamethods (so
                // tables with __lt sort correctly). Falls through to raw
                // comparison for numbers/strings (the common case).
                return sc.ev.lt_meta(a, b, sc.off);
            }
            ValueVec args{a, b};
            ValueVec r = sc.ev.call_value(sc.comp, std::move(args), sc.off);
            return !r.empty() && r[0].truthy();
        }

        static void sort_insertion(SortCtx& sc,
                                   std::vector<LuaValue>& a,
                                   long long lo, long long hi)
        {
            for (long long i = lo + 1; i <= hi; ++i) {
                LuaValue v = std::move(a[static_cast<std::size_t>(i)]);
                long long j = i - 1;
                while (j >= lo && sort_less(sc, v, a[static_cast<std::size_t>(j)])) {
                    a[static_cast<std::size_t>(j + 1)] =
                        std::move(a[static_cast<std::size_t>(j)]);
                    --j;
                }
                a[static_cast<std::size_t>(j + 1)] = std::move(v);
            }
        }

        // Median-of-3: sort a[lo], a[mid], a[hi] and place the median at
        // a[hi-1] as the pivot. Returns the pivot index (hi-1).
        static long long sort_median3(SortCtx& sc,
                                      std::vector<LuaValue>& a,
                                      long long lo, long long hi)
        {
            long long mid = lo + (hi - lo) / 2;
            if (sort_less(sc, a[static_cast<std::size_t>(mid)],
                          a[static_cast<std::size_t>(lo)]))
                std::swap(a[static_cast<std::size_t>(lo)],
                          a[static_cast<std::size_t>(mid)]);
            if (sort_less(sc, a[static_cast<std::size_t>(hi)],
                          a[static_cast<std::size_t>(mid)]))
                std::swap(a[static_cast<std::size_t>(hi)],
                          a[static_cast<std::size_t>(mid)]);
            if (sort_less(sc, a[static_cast<std::size_t>(mid)],
                          a[static_cast<std::size_t>(lo)]))
                std::swap(a[static_cast<std::size_t>(lo)],
                          a[static_cast<std::size_t>(mid)]);
            // Stash the median (pivot) at hi-1.
            std::swap(a[static_cast<std::size_t>(mid)],
                      a[static_cast<std::size_t>(hi - 1)]);
            return hi - 1;
        }

        static void sort_qsort(SortCtx& sc, std::vector<LuaValue>& a,
                               long long lo, long long hi)
        {
            while (lo + 16 < hi) {   // tail-recursion-eliminated: recurse the
                                     // SMALLER side, loop the larger
                long long p = sort_median3(sc, a, lo, hi);
                LuaValue pivot = a[static_cast<std::size_t>(p)];
                long long i = lo, j = hi - 1;
                for (;;) {
                    while (sort_less(sc,
                                     a[static_cast<std::size_t>(++i)], pivot)) {}
                    while (sort_less(sc,
                                     pivot, a[static_cast<std::size_t>(--j)])) {}
                    if (i < j)
                        std::swap(a[static_cast<std::size_t>(i)],
                                  a[static_cast<std::size_t>(j)]);
                    else
                        break;
                }
                // Restore pivot to position i.
                std::swap(a[static_cast<std::size_t>(i)],
                          a[static_cast<std::size_t>(hi - 1)]);

                // Recurse on the smaller side; loop on the larger (so worst-case
                // stack depth is O(log n)).
                if (i - lo < hi - i) {
                    sort_qsort(sc, a, lo, i - 1);
                    lo = i + 1;
                } else {
                    sort_qsort(sc, a, i + 1, hi);
                    hi = i - 1;
                }
            }
            if (lo < hi) sort_insertion(sc, a, lo, hi);
        }

        ValueVec b_sort(Evaluator& ev, ValueVec args)
        {
            Table* t = table_arg(args, 0, "sort");
            LuaValue comp = args.size() >= 2 ? args[1] : LuaValue::nil();
            if (!comp.is_nil() && !comp.is_callable())
                throw LuaError("bad argument #2 to 'sort' (function expected)", 0);

            long long n = table_len_meta(ev, t);

            // "too big": array length must fit a sane range. Lua rejects
            // lengths > INT_MAX. Negative lengths (e.g. from __len) are fine:
            // the sort loop just does nothing (n < 1 => no comparisons).
            if (n > static_cast<long long>(0x7FFFFFFF))
                throw LuaError("too big", 0);
            if (n < 1) return {};   // nothing to sort

            std::vector<LuaValue> a;
            a.reserve(static_cast<std::size_t>(n));
            for (long long i = 1; i <= n; ++i)
                a.push_back(arr_get(t, i));

            if (n >= 2) {
                SortCtx sc{ev, comp, 0};
                sort_qsort(sc, a, 0, n - 1);
                // Totalness check: comp(a,b) and comp(b,a) both true for some
                // adjacent pair means the ordering is incoherent.
                if (!comp.is_nil()) {
                    for (long long i = 1; i < n; ++i) {
                        if (sort_less(sc, a[static_cast<std::size_t>(i)],
                                      a[static_cast<std::size_t>(i - 1)]) &&
                            sort_less(sc, a[static_cast<std::size_t>(i - 1)],
                                      a[static_cast<std::size_t>(i)])) {
                            throw LuaError("invalid order function for sorting", 0);
                        }
                    }
                }
            }

            // Write back.
            for (long long i = 0; i < n; ++i)
                arr_set(t, i + 1, a[static_cast<std::size_t>(i)]);
            return {};
        }

        } // namespace tablib

        // -------------------------------------------------------------------
        // install_table_lib: create the `table` table, populate it.
        // -------------------------------------------------------------------
        void install_table_lib(Evaluator& ev)
        {
            Heap& h = ev.heap();
            Table* ttab = h.make_table();

            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = h.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                ttab->hash[k] = LuaValue::builtin(b);
            };

            using namespace tablib;
            add("insert", b_insert);
            add("remove", b_remove);
            add("concat", b_concat);
            add("pack",   b_pack);
            add("unpack", b_unpack);
            add("move",   b_move);
            add("sort",   b_sort);

            { LuaKey gk; gk.k = LuaKey::K::Str; gk.s = "table";
              ev.globals().hash[gk] = LuaValue::table(ttab); }
        }

    } // namespace lua
} // namespace ys
