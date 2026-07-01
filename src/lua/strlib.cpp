#include "lua/strlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

        // Fetch an integer argument with a default.
        static long long int_arg(const ValueVec& args, int idx,
                                 long long dflt)
        {
            if (static_cast<std::size_t>(idx) >= args.size())
                return dflt;
            if (!args[idx].is_int())
                throw LuaError("bad argument (number expected)", 0);
            return args[idx].as_int();
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
            std::size_t a = rel_pos(i, s.size());
            std::size_t b = rel_pos(j, s.size());
            if (b < a) return {mk_str(ev, "")};
            return {mk_str(ev, s.substr(a, b - a + 1))};
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
                if (c.len == 0) {
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

        // string.match(s, pat [, init]) — anchored at init, returns captures
        ValueVec b_match(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "match");
            const std::string& p = str_arg(ev, args, 1, "match");
            long long init_ll = args.size() >= 3 && args[2].is_int()
                ? args[2].as_int() : 1;
            std::size_t init = rel_pos(init_ll, s.size());

            try {
                // Strip anchor: match() tries only at init (like ^-anchored find).
                std::string_view pat{p};
                bool anchored = !pat.empty() && pat[0] == '^';
                std::string_view rp = anchored ? pat.substr(1) : pat;
                // Try at init only.
                auto mr = pattern_at(s, init, p);
                if (!mr) return {LuaValue::nil()};
                auto caps = caps_to_lua(ev, s, mr->captures);
                if (caps.empty()) {
                    // No captures: return the whole match
                    return {mk_str(ev, std::string(s.substr(init, mr->end - init)))};
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

        // string.gmatch(s, pat) -> iterator, state, nil
        ValueVec b_gmatch(Evaluator& ev, ValueVec args)
        {
            const std::string& s = str_arg(ev, args, 0, "gmatch");
            const std::string& p = str_arg(ev, args, 1, "gmatch");

            Table* state = ev.heap().make_table();
            LuaKey sk; sk.k = LuaKey::K::Str; sk.s = "s";
            state->hash[sk] = mk_str(ev, s);
            LuaKey pk; pk.k = LuaKey::K::Str; pk.s = "p";
            state->hash[pk] = mk_str(ev, p);

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
                        int idx = d - '0';
                        if (idx == 0) {
                            out += std::string(subj.substr(mstart, mend - mstart));
                        } else if (idx <= num_captures) {
                            const Capture& c = caps[idx - 1];
                            if (c.len == 0)
                                throw LuaError("invalid capture index in replacement string", 0);
                            out += std::string(subj.substr(c.start, c.len));
                        } else {
                            throw LuaError(std::string("invalid capture index %") +
                                           d + " in replacement string", 0);
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
            long long count = 0;

            while (i <= s.size() && (max_s < 0 || count < max_s)) {
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
                ++count;

                std::string replacement;
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
                        (rv[0].is_bool() && !rv[0].as_bool()))
                        replacement = std::string(s.substr(mstart, mend - mstart));
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
                        if (c.len == 0)
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
                    if (tval.is_nil() || (tval.is_bool() && !tval.as_bool()))
                        replacement = std::string(s.substr(mstart, mend - mstart));
                    else if (tval.is_str())
                        replacement = tval.as_str()->data;
                    else
                        throw LuaError("invalid replacement value (a " +
                                       std::string(type_name(tval)) + ")", 0);
                }
                else {
                    throw LuaError("bad argument #3 to 'gsub' (string/function/table expected)", 0);
                }

                result += replacement;
                if (mend > i)
                    i = mend;
                else {
                    if (i < s.size()) result += s[i];
                    ++i;
                }
            }
            if (i < s.size()) result += s.substr(i);
            return {mk_str(ev, std::move(result)),
                    LuaValue::integer(count)};
        }

        // -------------------------------------------------------------------
        // pack / unpack / packsize — deferred stubs
        // -------------------------------------------------------------------
        ValueVec b_pack(Evaluator&, ValueVec) { throw LuaError("string.pack: not yet implemented", 0); }
        ValueVec b_unpack(Evaluator&, ValueVec) { throw LuaError("string.unpack: not yet implemented", 0); }
        ValueVec b_packsize(Evaluator&, ValueVec) { throw LuaError("string.packsize: not yet implemented", 0); }

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
            ev.globals().vars["string"] = LuaValue::table(strtab);

            // Per-type string metatable: __index -> string table.
            Table* mt = h.make_table();
            LuaKey idx; idx.k = LuaKey::K::Str; idx.s = "__index";
            mt->hash[idx] = LuaValue::table(strtab);
            ev.string_metatable() = mt;
        }

    } // namespace lua
} // namespace ys
