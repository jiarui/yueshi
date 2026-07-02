#include "lua/strlib.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "lua/evaluator.h"
#include "lua/numops.h"
#include "lua/pattern.h"
#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        namespace strlib {

        // -------------------------------------------------------------------
        // Helpers
        // -------------------------------------------------------------------

        // Fetch a string argument's data, erroring if missing/wrong type.
        static const std::string& str_arg(Evaluator& ev, const ValueVec& args,
                                          int idx, const char* fn)
        {
            if (static_cast<std::size_t>(idx) >= args.size() || !args[idx].is_str()) {
                throw LuaError(std::string("bad argument #") +
                               std::to_string(idx + 1) + " to '" + fn +
                               "' (string expected)", 0);
            }
            (void)ev;
            return args[idx].as_str()->data;
        }

        // Fetch an integer argument with a default. Accepts floats whose
        // value is exactly integral (Lua 5.4 coerces them for these positions).
        static long long int_arg(const ValueVec& args, int idx,
                                 long long dflt)
        {
            if (static_cast<std::size_t>(idx) >= args.size())
                return dflt;
            const LuaValue& v = args[idx];
            if (v.is_int()) return v.as_int();
            if (v.is_flt()) {
                double f = v.as_flt();
                if (std::floor(f) == f &&
                    f >= static_cast<double>(std::numeric_limits<long long>::min()) &&
                    f <= static_cast<double>(std::numeric_limits<long long>::max()))
                    return static_cast<long long>(f);
            }
            throw LuaError("bad argument (number expected)", 0);
        }

        // Make a string value via the heap.
        static LuaValue mk_str(Evaluator& ev, std::string s)
        {
            return LuaValue::str(ev.heap().make_string(std::move(s)));
        }

        // Normalize a Lua 1-based index (may be negative = from end) to a
        // 0-based byte offset into [0, len]. Returns clamped result.
        static std::size_t rel_pos(long long i, std::size_t len)
        {
            if (i < 0) i += static_cast<long long>(len) + 1;
            if (i < 1) return 0;
            if (static_cast<std::size_t>(i) > len) return len;
            return static_cast<std::size_t>(i) - 1;
        }

        // -------------------------------------------------------------------
        // Basic string functions
        // -------------------------------------------------------------------

        ValueVec b_len(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "len");
            return {LuaValue::integer(static_cast<long long>(s.size()))};
        }

        ValueVec b_sub(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "sub");
            long long i = int_arg(args, 1, 1);
            long long j = int_arg(args, 2, static_cast<long long>(s.size()));
            // Lua's string.sub: normalize negative indices, then check i > j
            // BEFORE converting to 0-based. i=1,j=0 → i>j → "" (not the first
            // char, which the naive 0-based conversion would yield).
            long long n = static_cast<long long>(s.size());
            if (i < 0) i += n + 1;
            if (j < 0) j += n + 1;
            if (i < 1) i = 1;
            if (j > n) j = n;
            if (i > j) return {mk_str(ev, "")};
            return {mk_str(ev, s.substr(
                static_cast<std::size_t>(i - 1),
                static_cast<std::size_t>(j - i + 1)))};
        }

        ValueVec b_upper(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "upper");
            std::string r;
            r.reserve(s.size());
            for (unsigned char c : s) r.push_back(static_cast<char>(std::toupper(c)));
            return {mk_str(ev, std::move(r))};
        }

        ValueVec b_lower(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "lower");
            std::string r;
            r.reserve(s.size());
            for (unsigned char c : s) r.push_back(static_cast<char>(std::tolower(c)));
            return {mk_str(ev, std::move(r))};
        }

        ValueVec b_reverse(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "reverse");
            std::string r(s.rbegin(), s.rend());
            return {mk_str(ev, std::move(r))};
        }

        ValueVec b_rep(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "rep");
            long long n = int_arg(args, 1, 0);
            std::string sep;
            if (args.size() >= 3 && args[2].is_str())
                sep = args[2].as_str()->data;
            std::string r;
            if (n <= 0) return {mk_str(ev, "")};
            if (sep.empty()) {
                r.reserve(s.size() * static_cast<std::size_t>(n));
                for (long long k = 0; k < n; ++k) r += s;
            } else {
                for (long long k = 0; k < n; ++k) {
                    if (k > 0) r += sep;
                    r += s;
                }
            }
            return {mk_str(ev, std::move(r))};
        }

        ValueVec b_byte(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "byte");
            long long i = int_arg(args, 1, 1);
            long long j = int_arg(args, 2, i);
            std::size_t a = rel_pos(i, s.size());
            std::size_t b = rel_pos(j, s.size());
            ValueVec out;
            for (std::size_t k = a; k <= b && k < s.size(); ++k)
                out.push_back(LuaValue::integer(
                    static_cast<long long>(static_cast<unsigned char>(s[k]))));
            return out;
        }

        ValueVec b_char(Evaluator& ev, ValueVec args)
        {
            std::string r;
            for (const auto& v : args) {
                if (!v.is_int())
                    throw LuaError("bad argument to 'char' (number expected)", 0);
                long long c = v.as_int();
                if (c < 0 || c > 255)
                    throw LuaError("bad argument to 'char' (value out of range)", 0);
                r.push_back(static_cast<char>(static_cast<unsigned char>(c)));
            }
            return {mk_str(ev, std::move(r))};
        }

        // -------------------------------------------------------------------
        // string.format
        // -------------------------------------------------------------------

        // Render a double in hex-float format (%a/%A). Custom implementation
        // for exact reference-Lua output matching.
        static std::string fmt_hex_float(double val, bool upper)
        {
            const char* hex = upper ? "0123456789ABCDF" : "0123456789abcdef";
            const char* pfx = upper ? "0X" : "0x";
            const char* pexp = upper ? "P" : "p";
            if (std::isnan(val))
                return upper ? "-NAN" : "-nan";  // Lua uses signbit-aware
            if (std::isnan(val)) return upper ? "NAN" : "nan";
            if (std::isinf(val))
                return std::signbit(val) ? (upper ? "-INF" : "-inf")
                                          : (upper ? "INF" : "inf");

            // Decompose into sign, mantissa, exponent.
            bool neg = std::signbit(val);
            if (val == 0.0) {
                // Handle -0.0
                return neg ? (std::string{"-"} + pfx + "0" + pexp + "+0")
                           : (std::string{pfx} + "0" + pexp + "+0");
            }
            if (neg) val = -val;

            // Extract mantissa and exponent (base 2).
            int exp2 = 0;
            uint64_t mant = 0;
            std::memcpy(&mant, &val, sizeof(double));
            // IEEE 754: bit 63 = sign, bits 52-62 = exp, bits 0-51 = mant
            int raw_exp = static_cast<int>((mant >> 52) & 0x7FF);
            uint64_t raw_mant = mant & ((1ULL << 52) - 1);

            if (raw_exp == 0) {
                // Denormalized
                exp2 = 1 - 1023 - 51;
                while (raw_mant && (raw_mant & (1ULL << 51)) == 0) {
                    raw_mant <<= 1;
                    --exp2;
                }
                raw_mant &= (1ULL << 52) - 1;  // remove implied bit
            } else {
                exp2 = raw_exp - 1023 - 51;
            }
            // mant has 52 bits of fraction (implied leading 1 stripped)
            // We want "1.xxxxx" in hex, so group the bits into hex digits.
            // The leading 1 is implicit. The 52 fraction bits = 13 hex digits.
            std::string digits;
            // Prepend the implied 1
            uint64_t bits = (1ULL << 52) | raw_mant;  // 53-bit mantissa
            // In hex, 53 bits = 14 hex digits (56 bits, top 3 are 0).
            // We want: 1.[13 hex digits]
            // bits = 1.xxxxxxxxxxxxx (1 + 52 bits) = shift so top 4 bits = 1
            // Actually: the value is bits * 2^(exp2 - 52).
            // In hex float: 1.hhhhhhhhhhhhh × 2^exp
            // Each hex digit = 4 bits. The fractional part is 52 bits / 4 = 13 hex digits.
            char buf[32];
            // Print the integer part (always 1 for normalized)
            // Then 13 hex digits of fraction
            for (int i = 12; i >= 0; --i) {
                int nibble = static_cast<int>((raw_mant >> (i * 4)) & 0xF);
                buf[12 - i] = hex[nibble];
            }
            buf[13] = '\0';
            std::string frac(buf, 13);
            // Strip trailing zeros
            while (!frac.empty() && frac.back() == '0') frac.pop_back();

            std::string r;
            if (neg) r += "-";
            r += pfx;
            r += "1.";
            r += frac.empty() ? "0" : frac;
            r += pexp;
            // Exponent in decimal with sign
            char ebuf[32];
            std::snprintf(ebuf, sizeof(ebuf), "%+d", exp2 + 52 - 52);
            // Actually: value = 1.frac × 2^exp2_adjusted. The exponent for hex float
            // is: the value = mant × 2^(exp2), where mant is in [1,2).
            // exp2 was raw_exp - 1023 - 52, but we have the implied 1, so
            // value = (1 + raw_mant/2^52) × 2^(raw_exp-1023).
            // So the hex exponent is raw_exp - 1023.
            int hex_exp;
            if (raw_exp == 0) {
                // For denorms we normalized; recompute
                hex_exp = exp2 + 52;
            } else {
                hex_exp = raw_exp - 1023;
            }
            std::snprintf(ebuf, sizeof(ebuf), "%+d", hex_exp);
            r += ebuf;
            return r;
        }

        // Lua %q: produce a quoted string safe for load().
        static std::string fmt_quoted(Evaluator& ev, const LuaValue& v)
        {
            std::string r = "\"";
            if (v.is_str()) {
                for (unsigned char c : v.as_str()->data) {
                    if (c == '"')      r += "\\\"";
                    else if (c == '\\') r += "\\\\";
                    else if (c == '\n') r += "\\n";
                    else if (c == '\r') r += "\\r";
                    else if (c == '\0') r += "\\0";
                    else if (c < 32 || c >= 127) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\%03d",
                                      static_cast<int>(c));
                        r += buf;
                    } else r += static_cast<char>(c);
                }
            } else {
                // Non-string: render as a Lua literal via number_to_string
                // (nil -> nil, true/false -> true/false, numbers -> literal).
                // For tables/functions, error.
                if (v.is_nil()) return "nil";
                if (v.is_bool()) return v.as_bool() ? "true" : "false";
                if (v.is_number()) {
                    r.clear();
                    return number_to_string(v);
                }
                throw LuaError("invalid option (%q) to 'format' (no text)", 0);
            }
            r += "\"";
            (void)ev;
            return r;
        }

        // A dummy address for nil values in %p.
        static const char val_dummy = 0;

        // Parse and apply one format spec. Returns the rendered string.
        // advances `pi` past the spec. `arg_idx` is the current argument index.
        static std::string fmt_one(Evaluator& ev, std::string_view fmt,
                                   std::size_t& pi, const ValueVec& args,
                                   std::size_t& arg_idx)
        {
            // Parse: %[flags][width][.precision]conv
            std::size_t start = pi;
            std::string flags, width, prec;
            bool has_prec = false;

            // flags: - + space # 0
            while (pi < fmt.size()) {
                char c = fmt[pi];
                if (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0') {
                    flags += c;
                    ++pi;
                } else break;
            }
            // width: digits
            while (pi < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[pi]))) {
                width += fmt[pi];
                ++pi;
            }
            // precision: . digits
            if (pi < fmt.size() && fmt[pi] == '.') {
                has_prec = true;
                ++pi;
                while (pi < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[pi]))) {
                    prec += fmt[pi];
                    ++pi;
                }
            }

            if (pi >= fmt.size())
                throw LuaError("invalid conversion to 'format'", 0);

            char conv = fmt[pi];
            ++pi;

            // Build a C format string for standard conversions.
            auto c_fmt = [&]() {
                std::string s = "%";
                s += flags + width;
                if (has_prec) s += "." + prec;
                return s;
            };

            auto get_arg = [&](const char* who) -> const LuaValue& {
                if (arg_idx >= args.size())
                    throw LuaError(std::string("bad argument #") +
                                   std::to_string(arg_idx + 1) + " to 'format' "
                                   "(no value)", 0);
                (void)who;
                return args[arg_idx];
            };

            switch (conv) {
            case 'd': case 'i': {
                const LuaValue& v = get_arg("format");
                if (!v.is_int() && !v.is_flt())
                    throw LuaError("bad argument to 'format' (number expected)", 0);
                long long val = v.is_int() ? v.as_int()
                                           : static_cast<long long>(v.as_flt());
                std::string f = c_fmt() + "lld";
                char buf[64];
                std::snprintf(buf, sizeof(buf), f.c_str(), val);
                ++arg_idx;
                return buf;
            }
            case 'u': {
                const LuaValue& v = get_arg("format");
                if (!v.is_number())
                    throw LuaError("bad argument to 'format' (number expected)", 0);
                unsigned long long val = static_cast<unsigned long long>(
                    v.is_int() ? v.as_int()
                               : (v.is_flt() ? static_cast<long long>(v.as_flt()) : 0));
                std::string f = c_fmt() + "llu";
                char buf[64];
                std::snprintf(buf, sizeof(buf), f.c_str(), val);
                ++arg_idx;
                return buf;
            }
            case 'o': case 'x': case 'X': {
                const LuaValue& v = get_arg("format");
                if (!v.is_number())
                    throw LuaError("bad argument to 'format' (number expected)", 0);
                long long val = v.is_int() ? v.as_int()
                                           : static_cast<long long>(v.as_flt());
                std::string f = c_fmt();
                f += conv;
                f += "ll";
                // Fix order: %<flags><width>[.prec]Xll  -> need ll before conv
                // Actually snprintf needs %llX not %Xll. Rebuild properly:
                f = "%" + flags + width;
                if (has_prec) f += "." + prec;
                f += "ll";
                f += conv;
                char buf[64];
                std::snprintf(buf, sizeof(buf), f.c_str(), val);
                ++arg_idx;
                return buf;
            }
            case 'c': {
                const LuaValue& v = get_arg("format");
                if (!v.is_number())
                    throw LuaError("bad argument to 'format' (number expected)", 0);
                // Lua doesn't allow modifiers on %c
                if (!flags.empty() || has_prec)
                    throw LuaError("invalid conversion (invalid modifiers for 'c')", 0);
                long long val = v.is_int() ? v.as_int()
                                           : static_cast<long long>(v.as_flt());
                return std::string(1, static_cast<char>(
                    static_cast<unsigned char>(val)));
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': {
                const LuaValue& v = get_arg("format");
                if (!v.is_number())
                    throw LuaError("bad argument to 'format' (number expected)", 0);
                double val = v.is_int() ? static_cast<double>(v.as_int())
                                        : v.as_flt();
                std::string f = c_fmt() + conv;
                char buf[512];
                std::snprintf(buf, sizeof(buf), f.c_str(), val);
                ++arg_idx;
                return buf;
            }
            case 's': {
                const LuaValue& v = get_arg("format");
                std::string s = v.is_str() ? v.as_str()->data
                                           : value_to_string(v);
                if (has_prec && !prec.empty()) {
                    int p = std::stoi(prec);
                    if (static_cast<int>(s.size()) > p)
                        s = s.substr(0, static_cast<std::size_t>(p));
                }
                std::string f = c_fmt() + "s";
                char buf[4096];
                // For very large strings, snprintf may truncate; use manual
                // padding for safety.
                std::snprintf(buf, sizeof(buf), f.c_str(), s.c_str());
                ++arg_idx;
                return buf;
            }
            case 'a': case 'A': {
                const LuaValue& v = get_arg("format");
                if (!v.is_number())
                    throw LuaError("bad argument to 'format' (number expected)", 0);
                double val = v.is_int() ? static_cast<double>(v.as_int())
                                        : v.as_flt();
                ++arg_idx;
                return fmt_hex_float(val, conv == 'A');
            }
            case 'q': {
                if (!flags.empty() || !width.empty() || has_prec)
                    throw LuaError("invalid conversion (modifiers for 'q')", 0);
                std::string r = fmt_quoted(ev, get_arg("format"));
                ++arg_idx;
                return r;
            }
            case 'p': {
                const LuaValue& v = get_arg("format");
                const void* ptr = v.as_gc();
                if (!ptr) ptr = static_cast<const void*>(&val_dummy);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%p", ptr);
                ++arg_idx;
                return buf;
            }
            default:
                throw LuaError(std::string("invalid conversion '") + conv +
                               "' to 'format'", 0);
            }
            (void)start;
        }

        ValueVec b_format(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                throw LuaError("bad argument #1 to 'format' (string expected)", 0);
            const std::string& fmt = args[0].as_str()->data;
            std::string result;
            std::size_t arg_idx = 1;  // args[0] is the format string
            std::size_t i = 0;
            while (i < fmt.size()) {
                if (fmt[i] != '%') {
                    result += fmt[i];
                    ++i;
                    continue;
                }
                // %% -> literal %
                if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
                    result += '%';
                    i += 2;
                    continue;
                }
                ++i;  // skip %
                result += fmt_one(ev, fmt, i, args, arg_idx);
            }
            return {mk_str(ev, std::move(result))};
        }

        // -------------------------------------------------------------------
        // Pattern functions (Part E) and pack/unpack (Part F) — forward
        // declarations. Real implementations added below.
        // -------------------------------------------------------------------
        ValueVec b_find(Evaluator& ev, ValueVec args);
        ValueVec b_match(Evaluator& ev, ValueVec args);
        ValueVec b_gmatch(Evaluator& ev, ValueVec args);
        ValueVec b_gsub(Evaluator& ev, ValueVec args);
        ValueVec b_pack(Evaluator& ev, ValueVec args);
        ValueVec b_unpack(Evaluator& ev, ValueVec args);
        ValueVec b_packsize(Evaluator& ev, ValueVec args);
        // -------------------------------------------------------------------
        // Pattern wrappers: find / match / gmatch / gsub
        // -------------------------------------------------------------------
        // Convert captures to LuaValue vector (1-based position for position
        // captures, string for normal captures).
        static ValueVec caps_to_lua(Evaluator& ev, std::string_view subj,
                                     const std::vector<Capture>& caps)
        {
            ValueVec out;
            for (const auto& c : caps) {
                if (is_position_cap(c)) {
                    // Position capture: 1-based byte offset
                    out.push_back(LuaValue::integer(
                        static_cast<long long>(c.start + 1)));
                } else {
                    out.push_back(mk_str(ev,
                        std::string(subj.substr(c.start, c.len))));
                }
            }
            return out;
        }

        // string.find(s, pat [, init [, plain]])
        ValueVec b_find(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "find");
            const std::string& p = str_arg(ev, args, 1, "find");
            long long init_ll = args.size() >= 3 && args[2].is_int()
                ? args[2].as_int() : 1;
            bool plain = args.size() >= 4 && args[3].truthy();
            std::size_t init = rel_pos(init_ll, s.size());

            try {
                auto fr = pattern_find(s, init, p, plain);
                if (!fr) return {LuaValue::nil()};
                ValueVec out;
                out.push_back(LuaValue::integer(
                    static_cast<long long>(fr->start + 1)));  // 1-based
                out.push_back(LuaValue::integer(
                    static_cast<long long>(fr->end)));        // 1-based inclusive end
                auto caps = caps_to_lua(ev, s, fr->captures);
                for (auto& c : caps) out.push_back(std::move(c));
                return out;
            } catch (const PatternError& e) {
                throw LuaError(e.what(), 0);
            }
        }

        // string.match(s, pat [, init]) — returns captures or whole match.
        // Semantically like find but returns captures instead of positions.
        // Non-anchored patterns search forward from init (like find).
        ValueVec b_match(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "match");
            const std::string& p = str_arg(ev, args, 1, "match");
            long long init_ll = args.size() >= 3 && args[2].is_int()
                ? args[2].as_int() : 1;
            std::size_t init = rel_pos(init_ll, s.size());

            try {
                // Use pattern_find (handles anchoring + forward search).
                auto fr = pattern_find(s, init, p, false);
                if (!fr) return {LuaValue::nil()};
                auto caps = caps_to_lua(ev, s, fr->captures);
                if (caps.empty()) {
                    // No captures: return the whole match.
                    return {mk_str(ev, std::string(s.substr(
                        fr->start, fr->end - fr->start)))};
                }
                return caps;
            } catch (const PatternError& e) {
                throw LuaError(e.what(), 0);
            }
        }

        // gmatch iterator: state table holds {s=subject, p=pattern, pos=curpos}.
        // The generic-for calls iter(state, control_var) where control_var is
        // the first return value from the previous call. We store `pos` in the
        // state table (not in the control var) so the iterator is stateless
        // w.r.t. the control var. Returns captures... or nil to stop.
        ValueVec b_gmatch_iter(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_table()) return {LuaValue::nil()};
            Table* state = args[0].as_table();

            LuaKey sk; sk.k = LuaKey::K::Str; sk.s = "s";
            auto sit = state->hash.find(sk);
            if (sit == state->hash.end() || !sit->second.is_str())
                return {LuaValue::nil()};
            const std::string& subj = sit->second.as_str()->data;

            LuaKey pk; pk.k = LuaKey::K::Str; pk.s = "p";
            auto pit = state->hash.find(pk);
            if (pit == state->hash.end() || !pit->second.is_str())
                return {LuaValue::nil()};
            const std::string& pat = pit->second.as_str()->data;

            // Read current position from state table.
            std::size_t pos = 0;
            LuaKey ik; ik.k = LuaKey::K::Str; ik.s = "i";
            auto iit = state->hash.find(ik);
            if (iit != state->hash.end() && iit->second.is_int())
                pos = static_cast<std::size_t>(iit->second.as_int());

            std::size_t prev_emit = SIZE_MAX;
            LuaKey ek; ek.k = LuaKey::K::Str; ek.s = "e";
            auto eit = state->hash.find(ek);
            if (eit != state->hash.end() && eit->second.is_int())
                prev_emit = static_cast<std::size_t>(eit->second.as_int());

            bool anchored = !pat.empty() && pat[0] == '^';

            while (pos <= subj.size()) {
                try {
                    auto mr = pattern_at(subj, pos, pat);
                    if (mr) {
                        bool is_empty = (mr->end == pos);
                        if (is_empty && pos == prev_emit) {
                            ++pos; continue;  // suppress
                        }
                        // Update state table.
                        std::size_t new_pos = mr->end;
                        if (is_empty) ++new_pos;  // advance past empty match
                        state->hash[ik] = LuaValue::integer(
                            static_cast<long long>(new_pos));
                        state->hash[ek] = LuaValue::integer(
                            static_cast<long long>(mr->end));
                        // Return captures (or whole match).
                        auto caps = caps_to_lua(ev, subj, mr->captures);
                        if (caps.empty()) {
                            return {mk_str(ev, std::string(
                                subj.substr(pos, mr->end - pos)))};
                        }
                        return caps;
                    }
                } catch (const PatternError& e) {
                    throw LuaError(e.what(), 0);
                }
                if (anchored) break;
                ++pos;
            }
            return {LuaValue::nil()};
        }

        // string.gmatch(s, pat [, init]) -> iterator, state, nil
        ValueVec b_gmatch(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "gmatch");
            const std::string& p = str_arg(ev, args, 1, "gmatch");
            // Init: relative position, NOT clamped to [1, len+1]. gmatch's
            // loop condition (pos <= len) handles out-of-range naturally.
            long long init_ll = args.size() >= 3 && args[2].is_number()
                ? (args[2].is_int() ? args[2].as_int()
                                    : static_cast<long long>(args[2].as_flt()))
                : 1;
            // Convert to 0-based, handling negative (from end).
            std::size_t init;
            long long n = static_cast<long long>(s.size());
            if (init_ll < 0) init_ll = n + 1 + init_ll;
            if (init_ll < 1) init = 0;
            else init = static_cast<std::size_t>(init_ll - 1);

            Table* state = ev.heap().make_table();
            LuaKey sk; sk.k = LuaKey::K::Str; sk.s = "s";
            state->hash[sk] = mk_str(ev, s);
            LuaKey pk; pk.k = LuaKey::K::Str; pk.s = "p";
            state->hash[pk] = mk_str(ev, p);
            LuaKey ik; ik.k = LuaKey::K::Str; ik.s = "i";
            state->hash[ik] = LuaValue::integer(static_cast<long long>(init));

            Builtin* iter = ev.heap().make_builtin("gmatch_iter", b_gmatch_iter);
            return {LuaValue::builtin(iter), LuaValue::table(state),
                    LuaValue::nil()};
        }

        // --- gsub ---
        static std::string apply_str_repl(std::string_view repl,
                                          std::string_view subj,
                                          std::size_t mstart, std::size_t mend,
                                          const std::vector<Capture>& caps,
                                          int num_captures)
        {
            std::string out;
            for (std::size_t i = 0; i < repl.size(); ++i) {
                if (repl[i] == '%' && i + 1 < repl.size()) {
                    char d = repl[i + 1];
                    if (d == '%') { out += '%'; ++i; continue; }
                    if (d >= '0' && d <= '9') {
                        int idx = d - '0';   // 0=whole match, 1-9=captures
                        if (idx == 0) {
                            // %0: whole match
                            out += std::string(subj.substr(mstart, mend - mstart));
                        } else {
                            // %1-%9: capture[idx-1] (0-based). If idx-1 >= num_captures,
                            // fall back to the whole match when idx==1 (Lua's
                            // get_onecapture: i>=level and i==0 → whole match).
                            int ci = idx - 1;  // 0-based capture index
                            if (ci >= num_captures) {
                                if (ci == 0) {
                                    // %1 with no captures → whole match
                                    out += std::string(subj.substr(mstart, mend - mstart));
                                } else {
                                    throw LuaError(std::string(
                                        "invalid capture index %") +
                                        d + " in replacement string", 0);
                                }
                            } else {
                                const Capture& c = caps[ci];
                                if (is_position_cap(c)) {
                                    // Position capture: emit 1-based position.
                                    out += std::to_string(c.start + 1);
                                } else {
                                    out += std::string(subj.substr(c.start, c.len));
                                }
                            }
                        }
                        ++i; continue;
                    }
                    throw LuaError("invalid use of '%' in replacement string", 0);
                }
                out += repl[i];
            }
            return out;
        }

        ValueVec b_gsub(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "gsub");
            const std::string& p = str_arg(ev, args, 1, "gsub");
            const LuaValue& repl = args.size() >= 3 ? args[2] : LuaValue::nil();
            long long max_s = args.size() >= 4 && args[3].is_int()
                ? args[3].as_int() : -1;

            // ^ anchor: pattern only matches at the very start of the string.
            bool anchored = !p.empty() && p[0] == '^';

            std::string result;
            std::size_t i = 0;
            std::size_t prev_end = SIZE_MAX;
            long long count = 0;       // substitutions (return value)
            long long nmatch = 0;      // total matches (for max_s limit)

            while (i <= s.size() && (max_s < 0 || nmatch < max_s)) {
                // Anchored pattern: only match at position 0.
                if (anchored && i > 0) {
                    if (i < s.size()) result += s[i];
                    ++i;
                    continue;
                }
                std::optional<MatchResult> mr;
                try {
                    mr = pattern_at(s, i, p);
                } catch (const PatternError& e) {
                    throw LuaError(e.what(), 0);
                }

                bool suppressed_empty = mr && mr->end == i && i == prev_end;
                if (!mr || suppressed_empty) {
                    if (i < s.size()) result += s[i];
                    ++i;
                    continue;
                }

                std::size_t mstart = i;
                std::size_t mend = mr->end;
                int ncaps = static_cast<int>(mr->captures.size());
                prev_end = mend;
                ++nmatch;

                std::string replacement;
                bool substituted = true;  // default for string repl
                if (repl.is_str()) {
                    replacement = apply_str_repl(repl.as_str()->data, s,
                                                 mstart, mend, mr->captures, ncaps);
                }
                else if (repl.is_callable()) {
                    ValueVec call_args;
                    if (ncaps == 0)
                        call_args.push_back(mk_str(ev,
                            std::string(s.substr(mstart, mend - mstart))));
                    else {
                        auto caps = caps_to_lua(ev, s, mr->captures);
                        for (auto& c : caps) call_args.push_back(std::move(c));
                    }
                    ValueVec rv = ev.call_value(repl, std::move(call_args), 0);
                    if (rv.empty() || rv[0].is_nil() ||
                        (rv[0].is_bool() && !rv[0].as_bool())) {
                        // nil/false: no substitution, keep original match.
                        substituted = false;
                        replacement = std::string(s.substr(mstart, mend - mstart));
                    }
                    else if (rv[0].is_str())
                        replacement = rv[0].as_str()->data;
                    else
                        throw LuaError("invalid replacement value (a " +
                                       std::string(type_name(rv[0])) + ")", 0);
                }
                else if (repl.is_table()) {
                    LuaValue key;
                    if (ncaps == 0)
                        key = mk_str(ev, std::string(s.substr(mstart, mend - mstart)));
                    else {
                        const Capture& c = mr->captures[0];
                        if (is_position_cap(c))
                            key = LuaValue::integer(static_cast<long long>(c.start + 1));
                        else
                            key = mk_str(ev, std::string(s.substr(c.start, c.len)));
                    }
                    // Direct table lookup.
                    LuaKey lk;
                    if (key.is_str()) { lk.k = LuaKey::K::Str; lk.s = key.as_str()->data; }
                    else if (key.is_int()) { lk.k = LuaKey::K::Int; lk.i = key.as_int(); }
                    auto it = repl.as_table()->hash.find(lk);
                    LuaValue tval = (it != repl.as_table()->hash.end())
                        ? it->second : LuaValue::nil();
                    // __index fallback.
                    if (tval.is_nil() && repl.as_table()->metatable) {
                        Table* mt = repl.as_table()->metatable;
                        LuaKey idxk; idxk.k = LuaKey::K::Str; idxk.s = "__index";
                        auto mit = mt->hash.find(idxk);
                        if (mit != mt->hash.end()) {
                            if (mit->second.is_table()) {
                                auto iit = mit->second.as_table()->hash.find(lk);
                                if (iit != mit->second.as_table()->hash.end())
                                    tval = iit->second;
                            } else if (mit->second.is_callable()) {
                                ValueVec rv = ev.call_value(
                                    mit->second, {repl, key}, 0);
                                if (!rv.empty()) tval = rv[0];
                            }
                        }
                    }
                    if (tval.is_nil() || (tval.is_bool() && !tval.as_bool())) {
                        substituted = false;
                        replacement = std::string(s.substr(mstart, mend - mstart));
                    } else if (tval.is_str())
                        replacement = tval.as_str()->data;
                    else
                        throw LuaError("invalid replacement value (a " +
                                       std::string(type_name(tval)) + ")", 0);
                }
                else {
                    throw LuaError("bad argument #3 to 'gsub' (string/function/table expected)", 0);
                }

                if (substituted) ++count;
                result += replacement;
                if (mend > i)
                    i = mend;
                else {
                    if (i < s.size()) result += s[i];
                    ++i;
                }
            }
            if (i < s.size()) result += s.substr(i);
            // Optimization: if no substitutions were made, return the original
            // string (Lua does this so %p identity is preserved for no-op
            // gsubs). Return the ORIGINAL LuaValue, not a new allocation.
            if (count == 0)
                return {args[0], LuaValue::integer(0)};
            return {mk_str(ev, std::move(result)),
                    LuaValue::integer(count)};
        }

        // ===================================================================
        // string.pack / unpack / packsize
        //
        // Direct port of Lua 5.4's lstrlib.c pack machinery (the KOption
        // enum, Header, getoption/getdetails, packint/unpackint, and the
        // three entry points). Semantics must match reference Lua byte-for-
        // byte (tpack.lua's ~420 assertions check exact byte layouts and
        // error strings). Key design points preserved:
        //   - islittle + maxalign are MUTABLE HEADER STATE scanned left-to-
        //     right; they are NOT per-option attributes. This is the single
        //     most common porting bug.
        //   - Kchar ('c') is never aligned, even when size > 1.
        //   - 'X' borrows its alignment from the FOLLOWING option.
        //   - unpackint has three regimes (size <, ==, > SZINT); the '>'
        //     regime validates that high bytes are canonical sign-extension
        //     (all 0x00 or all 0xFF matching the decoded sign bit).
        //   - packsize overflow guard is `totalsize <= MAXSIZE - size`
        //     where MAXSIZE == INT_MAX (saturating add, NOT MAXSIZE/2).
        // ===================================================================
        namespace packimpl {

        // Platform constants (match reference Lua on 64-bit Linux/macOS).
        static constexpr int           MAXINTSIZE = 16;
        static constexpr int           NB         = 8;            // CHAR_BIT
        static constexpr unsigned char MC         = 0xFF;
        static constexpr int           SZINT      = sizeof(long long);  // 8
        static constexpr std::size_t   MAXSIZE    =
            static_cast<std::size_t>(std::numeric_limits<int>::max());
        // LUAI_MAXALIGN on 64-bit platforms is 8 (largest of double/long/ptr).
        static constexpr int           DFLT_ALIGN = sizeof(void*) > sizeof(double)
                                                    ? static_cast<int>(sizeof(void*))
                                                    : static_cast<int>(sizeof(double));

        // Native endianness: compile-time via C++20 std::endian.
        constexpr bool native_is_little() noexcept
        {
            return std::endian::native == std::endian::little;
        }

        struct Header {
            bool islittle;
            int  maxalign;
        };
        inline void initheader(Header& h) noexcept
        {
            h.islittle  = native_is_little();
            h.maxalign  = 1;
        }

        enum class KOption {
            Kint, Kuint, Kfloat, Knumber, Kdouble,
            Kchar, Kstring, Kzstr, Kpadding, Kpaddalign, Knop
        };

        // Throw a format-level error (arg #1 is always the format string for
        // these). All pack/unpack/packsize format errors are arg-1 errors.
        [[noreturn]] static void fmt_error(const std::string& msg)
        {
            throw LuaError("bad argument #1 to 'pack' (" + msg + ")", 0);
        }
        // Same, but for a different function name (unpack/packsize).
        [[noreturn]] static void fmt_error(const char* fn, const std::string& msg)
        {
            throw LuaError(std::string("bad argument #1 to '") + fn +
                           "' (" + msg + ")", 0);
        }

        // Parse optional decimal digits with overflow guard. `i` is advanced
        // past consumed digits. Returns `df` if no digits present.
        static int getnum(const std::string& fmt, std::size_t& i, int df)
        {
            auto is_digit = [](char c) { return '0' <= c && c <= '9'; };
            if (i >= fmt.size() || !is_digit(fmt[i])) return df;
            int a = 0;
            do {
                a = a * 10 + (fmt[i] - '0');
                ++i;
            } while (i < fmt.size() && is_digit(fmt[i]) &&
                     a <= static_cast<int>((MAXSIZE - 9) / 10));
            return a;
        }

        // getnum + range check [1, MAXINTSIZE]. Used by i/I/s/!.
        static int getnumlimit(Header& h, const std::string& fmt,
                               std::size_t& i, int df, const char* fn)
        {
            (void)h;
            int sz = getnum(fmt, i, df);
            if (sz > MAXINTSIZE || sz <= 0)
                fmt_error(fn, "integral size (" + std::to_string(sz) +
                          ") out of limits [1," + std::to_string(MAXINTSIZE) + "]");
            return sz;
        }

        // The directive dispatch. Returns the option kind and sets *psize.
        // Advances `i` past the directive char AND any trailing digits.
        // May mutate h (for <, >, =, !).
        static KOption getoption(Header& h, const std::string& fmt,
                                 std::size_t& i, int* psize, const char* fn)
        {
            if (i >= fmt.size()) fmt_error(fn, "invalid format");
            char opt = fmt[i++];
            switch (opt) {
            case 'b': *psize = sizeof(char);            return KOption::Kint;
            case 'B': *psize = sizeof(char);            return KOption::Kuint;
            case 'h': *psize = sizeof(short);           return KOption::Kint;
            case 'H': *psize = sizeof(short);           return KOption::Kuint;
            case 'l': *psize = sizeof(long);            return KOption::Kint;
            case 'L': *psize = sizeof(long);            return KOption::Kuint;
            case 'j': *psize = sizeof(long long);       return KOption::Kint;
            case 'J': *psize = sizeof(long long);       return KOption::Kuint;
            case 'T': *psize = sizeof(std::size_t);     return KOption::Kuint;
            case 'f': *psize = sizeof(float);           return KOption::Kfloat;
            case 'd': *psize = sizeof(double);          return KOption::Kdouble;
            // lua_Number is double for yueshi -> 'n' == 'd'.
            case 'n': *psize = sizeof(double);          return KOption::Knumber;
            case 'i': *psize = getnumlimit(h, fmt, i, sizeof(int),       fn); return KOption::Kint;
            case 'I': *psize = getnumlimit(h, fmt, i, sizeof(int),       fn); return KOption::Kuint;
            case 's': *psize = getnumlimit(h, fmt, i, sizeof(std::size_t), fn); return KOption::Kstring;
            case 'c': {
                int sz = getnum(fmt, i, -1);
                if (sz == -1)
                    fmt_error(fn, "missing size for format option 'c'");
                *psize = sz;
                return KOption::Kchar;
            }
            case 'z': *psize = 0;  return KOption::Kzstr;
            case 'x': *psize = 1;  return KOption::Kpadding;
            case 'X': *psize = 0;  return KOption::Kpaddalign;
            case ' ': *psize = 0;  return KOption::Knop;
            case '<': h.islittle = true;                 *psize = 0; return KOption::Knop;
            case '>': h.islittle = false;                *psize = 0; return KOption::Knop;
            case '=': h.islittle = native_is_little();   *psize = 0; return KOption::Knop;
            case '!': h.maxalign = getnumlimit(h, fmt, i, DFLT_ALIGN, fn);
                      *psize = 0; return KOption::Knop;
            default:
                fmt_error(fn, "invalid format option '" + std::string(1, opt) + "'");
            }
        }

        // Wraps getoption: handles 'X' lookahead, computes ntoalign (pad bytes
        // to insert BEFORE this option to reach the required alignment).
        static KOption getdetails(Header& h, std::size_t totalsize,
                                  const std::string& fmt, std::size_t& i,
                                  int* psize, int* ntoalign, const char* fn)
        {
            KOption opt = getoption(h, fmt, i, psize, fn);
            int align = *psize;   // usually alignment follows size
            if (opt == KOption::Kpaddalign) {
                // 'X' peeks the NEXT option's size as its alignment target.
                if (i >= fmt.size())
                    fmt_error(fn, "invalid next option for option 'X'");
                KOption next = getoption(h, fmt, i, &align, fn);
                if (next == KOption::Kchar || align == 0)
                    fmt_error(fn, "invalid next option for option 'X'");
            }
            if (align <= 1 || opt == KOption::Kchar) {
                *ntoalign = 0;   // 'c' is never aligned; size<=1 needs none
            } else {
                if (align > h.maxalign) align = h.maxalign;
                if ((align & (align - 1)) != 0)
                    fmt_error(fn, "format asks for alignment not power of 2");
                *ntoalign = (align - static_cast<int>(totalsize &
                            static_cast<std::size_t>(align - 1))) & (align - 1);
            }
            return opt;
        }

        // Write an integer as `size` bytes into `buf`, little or big endian.
        // `neg` triggers sign-extension (0xFF fill) for size > SZINT.
        static void packint(std::string& buf, unsigned long long n,
                            bool islittle, int size, bool neg)
        {
            std::size_t p = buf.size();
            buf.append(static_cast<std::size_t>(size), '\0');
            char* b = &buf[p];
            b[islittle ? 0 : size - 1] = static_cast<char>(n & MC);
            for (int k = 1; k < size; ++k) {
                n >>= NB;
                b[islittle ? k : size - 1 - k] = static_cast<char>(n & MC);
            }
            if (neg && size > SZINT) {
                for (int k = SZINT; k < size; ++k)
                    b[islittle ? k : size - 1 - k] = static_cast<char>(MC);
            }
        }

        // Copy `size` bytes from src to dest, byte-reversing if the target
        // endianness differs from native. Used for float/double.
        static void copywithendian(std::string& buf, const char* src,
                                   int size, bool islittle)
        {
            std::size_t p = buf.size();
            buf.append(static_cast<std::size_t>(size), '\0');
            char* dest = &buf[p];
            if (islittle == native_is_little()) {
                std::memcpy(dest, src, static_cast<std::size_t>(size));
            } else {
                for (int k = 0; k < size; ++k)
                    dest[size - 1 - k] = src[k];
            }
        }

        // Read `size` bytes as an integer (signed or unsigned). Three
        // regimes: size<SZINT sign-extends; size==SZINT passthrough;
        // size>SZINT validates that high bytes are canonical sign-extension.
        static long long unpackint(const char* str, bool islittle,
                                   int size, bool issigned, const char* fn)
        {
            unsigned long long res = 0;
            int limit = (size <= SZINT) ? size : SZINT;
            for (int k = limit - 1; k >= 0; --k) {
                res <<= NB;
                res |= static_cast<unsigned long long>(
                    static_cast<unsigned char>(str[islittle ? k : size - 1 - k]));
            }
            if (size < SZINT) {
                if (issigned) {
                    unsigned long long mask =
                        static_cast<unsigned long long>(1) << (size * NB - 1);
                    res = (res ^ mask) - mask;
                }
            } else if (size > SZINT) {
                unsigned char mask =
                    (!issigned || static_cast<long long>(res) >= 0) ? 0 : MC;
                for (int k = limit; k < size; ++k) {
                    if (static_cast<unsigned char>(
                            str[islittle ? k : size - 1 - k]) != mask)
                        fmt_error(fn, std::to_string(size) +
                                  "-byte integer does not fit into Lua Integer");
                }
            }
            return static_cast<long long>(res);
        }

        // 1-based position resolver for unpack's optional `pos` arg.
        // pos>0 -> pos; pos==0 -> 1; pos<-len -> 1; else len+pos+1.
        static std::size_t posrelatI(long long pos, std::size_t len) noexcept
        {
            if (pos > 0) return static_cast<std::size_t>(pos);
            if (pos == 0) return 1;
            if (pos < -static_cast<long long>(len)) return 1;
            return len + static_cast<std::size_t>(pos) + 1;
        }

        // Coerce an argument to a (Lua) integer for pack's Kint/Kuint cases.
        // Accepts ints and integral-valued floats; throws on bad type/range.
        static long long int_value(const ValueVec& args, std::size_t idx,
                                   const char* fn)
        {
            if (idx >= args.size())
                throw LuaError(std::string("bad argument #") +
                               std::to_string(idx + 1) + " to '" + fn +
                               "' (number expected)", 0);
            const LuaValue& v = args[idx];
            if (v.is_int()) return v.as_int();
            if (v.is_flt()) {
                double f = v.as_flt();
                if (std::floor(f) == f &&
                    f >= static_cast<double>(std::numeric_limits<long long>::min()) &&
                    f <= static_cast<double>(std::numeric_limits<long long>::max()))
                    return static_cast<long long>(f);
            }
            throw LuaError(std::string("bad argument #") +
                           std::to_string(idx + 1) + " to '" + fn +
                           "' (number has no integer representation)", 0);
        }

        // Coerce an argument to a double for pack's Kfloat/Kdouble/Knumber.
        static double num_value(const ValueVec& args, std::size_t idx,
                                const char* fn)
        {
            if (idx >= args.size() || !args[idx].is_number())
                throw LuaError(std::string("bad argument #") +
                               std::to_string(idx + 1) + " to '" + fn +
                               "' (number expected)", 0);
            return to_double(args[idx]);
        }

        // Fetch a string argument (1-based Lua arg index).
        static const std::string& str_value(const ValueVec& args,
                                            std::size_t idx, const char* fn)
        {
            if (idx >= args.size() || !args[idx].is_str())
                throw LuaError(std::string("bad argument #") +
                               std::to_string(idx + 1) + " to '" + fn +
                               "' (string expected)", 0);
            return args[idx].as_str()->data;
        }

        } // namespace packimpl

        // -------------------------------------------------------------------
        // string.packsize(fmt)
        // -------------------------------------------------------------------
        ValueVec b_packsize(Evaluator&, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                throw LuaError("bad argument #1 to 'packsize' (string expected)", 0);
            const std::string& fmt = args[0].as_str()->data;
            using namespace packimpl;
            Header h; initheader(h);
            std::size_t totalsize = 0;
            std::size_t i = 0;
            while (i < fmt.size()) {
                int size = 0, ntoalign = 0;
                KOption opt = getdetails(h, totalsize, fmt, i, &size, &ntoalign, "packsize");
                if (opt == KOption::Kstring || opt == KOption::Kzstr)
                    fmt_error("packsize", "variable-length format");
                std::size_t sz = static_cast<std::size_t>(size) +
                                 static_cast<std::size_t>(ntoalign);
                if (totalsize > MAXSIZE - sz)
                    fmt_error("packsize", "format result too large");
                totalsize += sz;
            }
            return {LuaValue::integer(static_cast<long long>(totalsize))};
        }

        // -------------------------------------------------------------------
        // string.pack(fmt, ...)
        // -------------------------------------------------------------------
        ValueVec b_pack(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                throw LuaError("bad argument #1 to 'pack' (string expected)", 0);
            const std::string& fmt = args[0].as_str()->data;
            using namespace packimpl;
            Header h; initheader(h);
            std::string buf;
            std::size_t arg_idx = 1;   // args[1] is the first value
            std::size_t i = 0;
            while (i < fmt.size()) {
                int size = 0, ntoalign = 0;
                KOption opt = getdetails(h, buf.size(), fmt, i, &size, &ntoalign, "pack");
                // Emit alignment pad bytes (LUAL_PACKPADBYTE == 0).
                buf.append(static_cast<std::size_t>(ntoalign), '\0');
                switch (opt) {
                case KOption::Kint: {
                    long long n = int_value(args, arg_idx, "pack");
                    if (size < SZINT) {
                        long long lim = static_cast<long long>(1) << (size * NB - 1);
                        if (!(-lim <= n && n < lim))
                            throw LuaError("bad argument #" + std::to_string(arg_idx + 1) +
                                           " to 'pack' (integer overflow)", 0);
                    }
                    packint(buf, static_cast<unsigned long long>(n),
                            h.islittle, size, n < 0);
                    ++arg_idx;
                    break;
                }
                case KOption::Kuint: {
                    long long n = int_value(args, arg_idx, "pack");
                    if (size < SZINT) {
                        unsigned long long lim =
                            (static_cast<unsigned long long>(1) << (size * NB));
                        if (!(static_cast<unsigned long long>(n) < lim))
                            throw LuaError("bad argument #" + std::to_string(arg_idx + 1) +
                                           " to 'pack' (unsigned overflow)", 0);
                    }
                    packint(buf, static_cast<unsigned long long>(n),
                            h.islittle, size, false);
                    ++arg_idx;
                    break;
                }
                case KOption::Kfloat: {
                    float f = static_cast<float>(num_value(args, arg_idx, "pack"));
                    copywithendian(buf, reinterpret_cast<const char*>(&f),
                                   sizeof(f), h.islittle);
                    ++arg_idx;
                    break;
                }
                case KOption::Knumber:
                case KOption::Kdouble: {
                    double d = num_value(args, arg_idx, "pack");
                    copywithendian(buf, reinterpret_cast<const char*>(&d),
                                   sizeof(d), h.islittle);
                    ++arg_idx;
                    break;
                }
                case KOption::Kchar: {
                    const std::string& s = str_value(args, arg_idx, "pack");
                    if (s.size() > static_cast<std::size_t>(size))
                        throw LuaError("bad argument #" + std::to_string(arg_idx + 1) +
                                       " to 'pack' (string longer than given size)", 0);
                    buf.append(s);
                    // Zero-pad to fixed length.
                    buf.append(static_cast<std::size_t>(size) - s.size(), '\0');
                    ++arg_idx;
                    break;
                }
                case KOption::Kstring: {
                    const std::string& s = str_value(args, arg_idx, "pack");
                    // Check the length fits in the size-prefix's range.
                    // (size * NB can be >= 64; shifting by 64 is UB, so guard.)
                    int bits = size * NB;
                    unsigned long long max_len =
                        (bits >= 64) ? ~0ULL
                                     : ((static_cast<unsigned long long>(1) << bits) - 1);
                    if (s.size() > max_len)
                        throw LuaError("bad argument #" + std::to_string(arg_idx + 1) +
                                       " to 'pack' (string length does not fit in given size)", 0);
                    // Length prefix (always unsigned, no overflow check).
                    packint(buf, static_cast<unsigned long long>(s.size()),
                            h.islittle, size, false);
                    buf.append(s);
                    ++arg_idx;
                    break;
                }
                case KOption::Kzstr: {
                    const std::string& s = str_value(args, arg_idx, "pack");
                    if (s.find('\0') != std::string::npos)
                        throw LuaError("bad argument #" + std::to_string(arg_idx + 1) +
                                       " to 'pack' (string contains zeros)", 0);
                    buf.append(s);
                    buf.push_back('\0');
                    ++arg_idx;
                    break;
                }
                case KOption::Kpadding:
                    // One zero byte (emitted below via the size==1 path).
                    buf.push_back('\0');
                    break;
                case KOption::Kpaddalign:
                    // ntoalign pad bytes already emitted above; Kpaddalign
                    // itself contributes no further bytes.
                    break;
                case KOption::Knop:
                    break;
                }
            }
            return {mk_str(ev, std::move(buf))};
        }

        // -------------------------------------------------------------------
        // string.unpack(fmt, s [, pos])
        // -------------------------------------------------------------------
        ValueVec b_unpack(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                throw LuaError("bad argument #1 to 'unpack' (string expected)", 0);
            if (args.size() < 2 || !args[1].is_str())
                throw LuaError("bad argument #2 to 'unpack' (string expected)", 0);
            const std::string& fmt  = args[0].as_str()->data;
            const std::string& data = args[1].as_str()->data;
            using namespace packimpl;
            Header h; initheader(h);
            std::size_t ld = data.size();
            long long parg = (args.size() >= 3 && args[2].is_number())
                             ? static_cast<long long>(to_double(args[2])) : 1;
            std::size_t pos = posrelatI(parg, ld) - 1;
            if (pos > ld)
                throw LuaError("bad argument #3 to 'unpack' (initial position out of string)", 0);
            ValueVec out;
            std::size_t i = 0;
            while (i < fmt.size()) {
                int size = 0, ntoalign = 0;
                KOption opt = getdetails(h, pos, fmt, i, &size, &ntoalign, "unpack");
                // Bounds: ntoalign + size must fit in the remaining data.
                if (static_cast<std::size_t>(ntoalign) + static_cast<std::size_t>(size) > ld - pos)
                    throw LuaError("bad argument #2 to 'unpack' (data string too short)", 0);
                pos += static_cast<std::size_t>(ntoalign);
                const char* d = data.data() + pos;
                switch (opt) {
                case KOption::Kint:
                    out.push_back(LuaValue::integer(
                        unpackint(d, h.islittle, size, true, "unpack")));
                    break;
                case KOption::Kuint:
                    out.push_back(LuaValue::integer(
                        unpackint(d, h.islittle, size, false, "unpack")));
                    break;
                case KOption::Kfloat: {
                    float f;
                    if (h.islittle == native_is_little())
                        std::memcpy(&f, d, sizeof(f));
                    else
                        for (int k = 0; k < static_cast<int>(sizeof(f)); ++k)
                            (reinterpret_cast<char*>(&f))[sizeof(f) - 1 - k] = d[k];
                    out.push_back(LuaValue::flt(static_cast<double>(f)));
                    break;
                }
                case KOption::Knumber:
                case KOption::Kdouble: {
                    double dbl;
                    if (h.islittle == native_is_little())
                        std::memcpy(&dbl, d, sizeof(dbl));
                    else
                        for (int k = 0; k < static_cast<int>(sizeof(dbl)); ++k)
                            (reinterpret_cast<char*>(&dbl))[sizeof(dbl) - 1 - k] = d[k];
                    out.push_back(LuaValue::flt(dbl));
                    break;
                }
                case KOption::Kchar:
                    out.push_back(mk_str(ev, std::string(d, static_cast<std::size_t>(size))));
                    break;
                case KOption::Kstring: {
                    unsigned long long len = static_cast<unsigned long long>(
                        unpackint(d, h.islittle, size, false, "unpack"));
                    if (static_cast<std::size_t>(size) + len > ld - pos)
                        throw LuaError("bad argument #2 to 'unpack' (data string too short)", 0);
                    out.push_back(mk_str(ev, std::string(d + size, static_cast<std::size_t>(len))));
                    pos += static_cast<std::size_t>(len);
                    break;
                }
                case KOption::Kzstr: {
                    std::size_t z = pos;
                    while (z < ld && data[z] != '\0') ++z;
                    if (z >= ld)
                        throw LuaError("bad argument #2 to 'unpack' (unfinished string for format 'z')", 0);
                    out.push_back(mk_str(ev, std::string(data, pos, z - pos)));
                    pos = z + 1;   // skip the \0
                    break;
                }
                case KOption::Kpadding:
                case KOption::Kpaddalign:
                case KOption::Knop:
                    break;
                }
                pos += static_cast<std::size_t>(size);
            }
            // Final position (1-based) is the last return value.
            out.push_back(LuaValue::integer(static_cast<long long>(pos) + 1));
            return out;
        }

        } // namespace strlib

        // -------------------------------------------------------------------
        // install_string_lib: create the `string` table, populate it, wire up
        // the per-type string metatable.
        // -------------------------------------------------------------------
        void install_string_lib(Evaluator& ev)
        {
            Heap& h = ev.heap();
            Table* strtab = h.make_table();

            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = h.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                strtab->hash[k] = LuaValue::builtin(b);
            };

            using namespace strlib;
            add("len",      b_len);
            add("sub",      b_sub);
            add("upper",    b_upper);
            add("lower",    b_lower);
            add("reverse",  b_reverse);
            add("rep",      b_rep);
            add("byte",     b_byte);
            add("char",     b_char);
            add("format",   b_format);
            add("find",     b_find);
            add("match",    b_match);
            add("gmatch",   b_gmatch);
            add("gsub",     b_gsub);
            add("pack",     b_pack);
            add("unpack",   b_unpack);
            add("packsize", b_packsize);

            // Bind `string` into globals.
            { LuaKey gk; gk.k = LuaKey::K::Str; gk.s = "string";
              ev.globals().hash[gk] = LuaValue::table(strtab); }

            // Per-type string metatable: __index -> string table.
            Table* mt = h.make_table();
            LuaKey idx; idx.k = LuaKey::K::Str; idx.s = "__index";
            mt->hash[idx] = LuaValue::table(strtab);
            ev.string_metatable() = mt;
        }

    } // namespace lua
} // namespace ys
