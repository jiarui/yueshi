#include "lua/utf8lib.h"

#include <cstdint>
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
        namespace utf8lib {

        // -------------------------------------------------------------------
        // Position helper: Lua-style relative index.
        //   pos >= 1  -> absolute (1-based)
        //   pos == 0  -> invalid (returns 0)
        //   pos < 0   -> from end: #s + 1 + pos  (so -1 = last)
        //   pos < -#s -> too negative (returns 0, treated invalid by caller)
        // Returns 0 if invalid.
        // -------------------------------------------------------------------
        static long long posrelat(long long pos, long long len) noexcept
        {
            if (pos >= 1) return pos;
            if (pos == 0) return 0;             // invalid
            if (pos < -len) return 0;           // too negative
            return len + 1 + pos;               // from end
        }

        // -------------------------------------------------------------------
        // UTF-8 codec
        // -------------------------------------------------------------------
        // Lead byte classification.
        enum { L1 = 1, L2, L3, L4, L5, L6, LINVALID };
        static int lead_len(unsigned char c) noexcept
        {
            if (c < 0x80)      return L1;
            if (c < 0xC2)      return LINVALID;   // 0x80-0xC1: invalid/overlong
            if (c < 0xE0)      return L2;
            if (c < 0xF0)      return L3;
            if (c < 0xF8)      return L4;
            if (c < 0xFC)      return L5;
            if (c < 0xFE)      return L6;
            return LINVALID;                      // 0xFE-0xFF
        }

        // Decode one codepoint starting at byte offset `i` in `s`.
        // On success: sets *cp, returns the byte length.
        // On invalid: returns -1.
        // `lax`: if true, accept surrogates (D800-DFFF) and values > 10FFFF
        // up to 0x7FFFFFFF (the original UTF-8 range, 5-6 byte sequences).
        static int decode_one(const std::string& s, std::size_t i,
                              std::uint32_t* cp, bool lax)
        {
            if (i >= s.size()) return 0;   // EOF
            unsigned char c = static_cast<unsigned char>(s[i]);
            int len = lead_len(c);
            if (len == LINVALID) return -1;
            if (len == L5 || len == L6) {
                if (!lax) return -1;   // 5-6 byte only in lax mode
            }
            if (i + static_cast<std::size_t>(len) > s.size()) return -1;

            // Extract payload.
            std::uint32_t r;
            unsigned char mask = static_cast<unsigned char>(
                (len == L1) ? 0x7F :
                (len == L2) ? 0x1F :
                (len == L3) ? 0x0F :
                (len == L4) ? 0x07 :
                (len == L5) ? 0x03 : 0x01);
            r = c & mask;
            for (int j = 1; j < len; ++j) {
                unsigned char cc = static_cast<unsigned char>(s[i + j]);
                if ((cc & 0xC0) != 0x80) return -1;
                r = (r << 6) | (cc & 0x3F);
            }

            // Overlong check (always enforced, even in lax mode).
            static const std::uint32_t min_val[] = {0, 0, 0x80, 0x800, 0x10000,
                                                     0x200000, 0x4000000};
            if (r < min_val[len]) return -1;

            // Surrogate range (D800-DFFF): invalid in strict, valid in lax.
            if (!lax && r >= 0xD800 && r <= 0xDFFF) return -1;

            // Above max: strict rejects > 0x10FFFF; lax allows up to 0x7FFFFFFF.
            if (!lax && r > 0x10FFFF) return -1;
            if (lax && r > 0x7FFFFFFF) return -1;

            *cp = r;
            return len;
        }

        // Encode a codepoint. Accepts [0, 0x7FFFFFFF] (original UTF-8 range).
        // Returns false if cp is invalid (surrogate, > 0x7FFFFFFF).
        static bool encode_one(std::uint32_t cp, std::string& out)
        {
            if (cp > 0x7FFFFFFF) return false;
            // Note: utf8.char DOES accept surrogates (the test at line 169-170
            // encodes D800/DFFF with the lax flag on codepoint, and char has
            // no lax flag — it always accepts the full original range). Lua
            // rejects only values > 0x7FFFFFFF or < 0.
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x200000) {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x4000000) {
                out.push_back(static_cast<char>(0xF8 | (cp >> 24)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 18) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xFC | (cp >> 30)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 24) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 18) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            return true;
        }

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

        // Extract the optional lax boolean (last argument). Defaults false.
        static bool lax_arg(const ValueVec& args, std::size_t idx) noexcept
        {
            return idx < args.size() && args[idx].truthy();
        }

        // Check if byte at position i (0-based) is a UTF-8 continuation byte.
        static bool is_cont(const std::string& s, std::size_t i) noexcept
        {
            if (i >= s.size()) return false;
            return (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80;
        }

        // -------------------------------------------------------------------
        // utf8.len(s [, i [, j [, lax]]])
        // -------------------------------------------------------------------
        ValueVec b_len(Evaluator&, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                bad_arg("len", 0, "string", args.empty() ? LuaValue::nil() : args[0]);
            const std::string& s = args[0].as_str()->data;
            long long len = static_cast<long long>(s.size());

            long long posi = args.size() >= 2 && args[1].is_number()
                ? posrelat(args[1].is_int() ? args[1].as_int()
                                            : static_cast<long long>(args[1].as_flt()),
                           len)
                : 1;
            long long posj = args.size() >= 3 && args[2].is_number()
                ? posrelat(args[2].is_int() ? args[2].as_int()
                                            : static_cast<long long>(args[2].as_flt()),
                           len)
                : len;
            bool lax = lax_arg(args, 3);

            // Empty range (posi > posj): return 0.
            if (posi > posj) return {LuaValue::integer(0)};

            // Bounds: i in [1, len+1], j in [0, len]. (i=len+1 gives empty range.)
            if (posi < 1 || posi > len + 1 || posj < 0 || posj > len)
                throw LuaError("string slice out of bounds", 0);

            long long count = 0;
            std::size_t p = static_cast<std::size_t>(posi - 1);
            std::size_t end = static_cast<std::size_t>(posj);
            while (p < end) {
                std::uint32_t cp;
                int n = decode_one(s, p, &cp, lax);
                if (n < 0)
                    return {LuaValue::nil(),
                            LuaValue::integer(static_cast<long long>(p) + 1)};
                if (n == 0) break;
                p += static_cast<std::size_t>(n);
                ++count;
            }
            return {LuaValue::integer(count)};
        }

        // -------------------------------------------------------------------
        // utf8.offset(s, n [, i])
        // -------------------------------------------------------------------
        ValueVec b_offset(Evaluator&, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                bad_arg("offset", 0, "string", args.empty() ? LuaValue::nil() : args[0]);
            const std::string& s = args[0].as_str()->data;
            long long len = static_cast<long long>(s.size());
            if (args.size() < 2 || !args[1].is_number())
                bad_arg("offset", 1, "number", args.size() < 2 ? LuaValue::nil() : args[1]);
            long long n = args[1].is_int() ? args[1].as_int()
                           : static_cast<long long>(args[1].as_flt());

            // Default i depends on sign of n: forward → 1, backward → len+1.
            bool explicit_i = args.size() >= 3 && args[2].is_number();
            long long raw_i = explicit_i
                ? (args[2].is_int() ? args[2].as_int()
                                    : static_cast<long long>(args[2].as_flt()))
                : (n >= 0 ? 1 : len + 1);
            long long i = posrelat(raw_i, len);
            if (i == 0) i = (raw_i < 0) ? 0 : len + 2;   // force out-of-bounds

            // Bounds check on i: valid range [1, len+1].
            if (i < 1 || i > len + 1) {
                if (explicit_i)
                    throw LuaError("initial position out of bounds", 0);
                return {LuaValue::nil()};
            }

            std::size_t pos = static_cast<std::size_t>(i - 1);

            // n == 0: round down to the codepoint containing pos.
            if (n == 0) {
                while (pos > 0 && is_cont(s, pos)) --pos;
                return {LuaValue::integer(static_cast<long long>(pos) + 1)};
            }

            // For n != 0, pos must be at a codepoint start (not a cont byte).
            // Exception: pos == len (one past end) is OK for backward.
            if (pos < s.size() && is_cont(s, pos))
                throw LuaError("continuation byte", 0);

            if (n > 0) {
                // Advance (n-1) codepoints. n=1 → current codepoint (pos itself).
                for (long long k = 0; k < n - 1; ++k) {
                    if (pos >= s.size()) return {LuaValue::nil()};
                    std::uint32_t cp;
                    int nb = decode_one(s, pos, &cp, true);   // lax: just walk
                    if (nb <= 0) return {LuaValue::nil()};
                    pos += static_cast<std::size_t>(nb);
                }
                if (pos > s.size()) return {LuaValue::nil()};
                return {LuaValue::integer(static_cast<long long>(pos) + 1)};
            } else {
                // Backward: go back |n| codepoints from pos.
                for (long long k = 0; k < -n; ++k) {
                    if (pos == 0) return {LuaValue::nil()};
                    --pos;
                    while (pos > 0 && is_cont(s, pos)) --pos;
                }
                // pos is now at a lead byte (or 0). Validate.
                if (pos < s.size() && is_cont(s, pos))
                    throw LuaError("continuation byte", 0);
                return {LuaValue::integer(static_cast<long long>(pos) + 1)};
            }
        }

        // -------------------------------------------------------------------
        // utf8.codepoint(s [, i [, j [, lax]]])
        // -------------------------------------------------------------------
        ValueVec b_codepoint(Evaluator&, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                bad_arg("codepoint", 0, "string", args.empty() ? LuaValue::nil() : args[0]);
            const std::string& s = args[0].as_str()->data;
            long long len = static_cast<long long>(s.size());

            long long posi = args.size() >= 2 && args[1].is_number()
                ? posrelat(args[1].is_int() ? args[1].as_int()
                                            : static_cast<long long>(args[1].as_flt()),
                           len)
                : 1;
            long long posj = args.size() >= 3 && args[2].is_number()
                ? posrelat(args[2].is_int() ? args[2].as_int()
                                            : static_cast<long long>(args[2].as_flt()),
                           len)
                : posi;
            bool lax = lax_arg(args, 3);

            // Empty range (posi > posj): return nothing. Must check before
            // bounds validation so that e.g. codepoint("", 1, -1) returns {}
            // rather than erroring.
            if (posi > posj) return {};

            // Bounds: i, j in [1, len]. (Stricter than len: no len+1 for i.)
            if (posi < 1 || posi > len || posj < 1 || posj > len)
                throw LuaError("string slice out of bounds", 0);

            ValueVec out;
            std::size_t p = static_cast<std::size_t>(posi - 1);
            std::size_t end = static_cast<std::size_t>(posj);   // 1-based, exclusive in 0-based
            while (p < end && p < s.size()) {
                std::uint32_t cp;
                int nb = decode_one(s, p, &cp, lax);
                if (nb < 0)
                    throw LuaError("invalid UTF-8 code", 0);
                if (nb == 0) break;
                out.push_back(LuaValue::integer(static_cast<long long>(cp)));
                p += static_cast<std::size_t>(nb);
            }
            return out;
        }

        // -------------------------------------------------------------------
        // utf8.char(...)
        // -------------------------------------------------------------------
        ValueVec b_char(Evaluator& ev, ValueVec args)
        {
            std::string out;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (!args[i].is_number())
                    bad_arg("char", static_cast<int>(i), "number", args[i]);
                std::uint32_t cp = args[i].is_int()
                    ? static_cast<std::uint32_t>(args[i].as_int())
                    : static_cast<std::uint32_t>(args[i].as_flt());
                if (!encode_one(cp, out))
                    throw LuaError("value out of range", 0);
            }
            return {LuaValue::str(ev.heap().make_string(std::move(out)))};
        }

        // -------------------------------------------------------------------
        // utf8.codes(s [, lax]) -> iterator for generic-for.
        // Iterator: f(s, prev_pos) -> (current_pos, codepoint) | nil
        // The control variable is the 1-based start position of the LAST
        // decoded codepoint (0 = initial). To advance, the iterator
        // re-decodes the codepoint at prev_pos to find its byte length,
        // then decodes the next codepoint.
        // -------------------------------------------------------------------
        static ValueVec codes_iter_impl(Evaluator&, const ValueVec& a, bool lax)
        {
            if (a.size() < 2 || !a[0].is_str()) return {LuaValue::nil()};
            const std::string& s = a[0].as_str()->data;
            long long prev = a[1].is_int() ? a[1].as_int() : 0;
            if (prev < 0) return {LuaValue::nil()};   // negative control → done

            std::size_t idx;
            if (prev == 0) {
                idx = 0;   // initial: start of string
            } else {
                // Advance past the codepoint at prev (1-based → 0-based prev-1).
                if (prev > static_cast<long long>(s.size())) return {LuaValue::nil()};
                std::uint32_t tmp;
                int n = decode_one(s, static_cast<std::size_t>(prev - 1), &tmp, true);
                if (n <= 0) return {LuaValue::nil()};
                idx = static_cast<std::size_t>(prev - 1) + static_cast<std::size_t>(n);
            }

            if (idx >= s.size()) return {LuaValue::nil()};
            std::uint32_t cp;
            int nb = decode_one(s, idx, &cp, lax);
            if (nb < 0) throw LuaError("invalid UTF-8 code", 0);
            if (nb == 0) return {LuaValue::nil()};
            return {LuaValue::integer(static_cast<long long>(idx) + 1),
                    LuaValue::integer(static_cast<long long>(cp))};
        }

        ValueVec b_codes(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                bad_arg("codes", 0, "string", args.empty() ? LuaValue::nil() : args[0]);
            LuaValue sv = args[0];
            bool lax = lax_arg(args, 1);
            BuiltinFn fn = lax
                ? +[](Evaluator& e, ValueVec a) -> ValueVec {
                      return codes_iter_impl(e, a, true);
                  }
                : +[](Evaluator& e, ValueVec a) -> ValueVec {
                      return codes_iter_impl(e, a, false);
                  };
            Builtin* iter = ev.heap().make_builtin("codes_iter", fn);
            // Initial control: 0 (signals "start from beginning").
            return {LuaValue::builtin(iter), sv, LuaValue::integer(0)};
        }

        } // namespace utf8lib

        // -------------------------------------------------------------------
        // install_utf8_lib
        // -------------------------------------------------------------------
        void install_utf8_lib(Evaluator& ev)
        {
            Heap& h = ev.heap();
            Table* t = h.make_table();
            using namespace utf8lib;

            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = h.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                t->hash[k] = LuaValue::builtin(b);
            };

            add("len",        b_len);
            add("offset",     b_offset);
            add("codepoint",  b_codepoint);
            add("char",       b_char);
            add("codes",      b_codes);

            // charpattern: matches a single UTF-8 codepoint in the original
            // UTF-8 range (Lua 5.4 uses \xFD to cover up to 6-byte sequences).
            // Contains a literal NUL byte; use explicit-length construction.
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "charpattern";
                static const char cp[] = "[\0-\x7F\xC2-\xFD][\x80-\xBF]*";
                constexpr int cp_len = sizeof(cp) - 1;
                t->hash[k] = LuaValue::str(h.make_string(
                    std::string(cp, cp_len)));
            }

            LuaKey gk; gk.k = LuaKey::K::Str; gk.s = "utf8";
            ev.globals().hash[gk] = LuaValue::table(t);
        }

    } // namespace lua
} // namespace ys
