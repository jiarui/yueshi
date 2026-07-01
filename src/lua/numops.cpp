#include "lua/numops.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

#include "lua/evaluator.h"   // LuaError
#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        // -------------------------------------------------------------------
        // Conversions + rendering
        // -------------------------------------------------------------------
        double to_double(const LuaValue& v) noexcept
        {
            return v.is_int() ? static_cast<double>(v.as_int()) : v.as_flt();
        }

        std::string number_to_string(const LuaValue& v)
        {
            if (v.is_int()) {
                return std::to_string(v.as_int());
            }
            double f = v.as_flt();
            if (std::isnan(f)) return std::signbit(f) ? "-nan" : "nan";
            if (std::isinf(f)) return std::signbit(f) ? "-inf" : "inf";
            // %.14g strips trailing noise but may drop the decimal point for
            // integral floats (e.g. 3.0 -> "3"). Lua guarantees a float always
            // shows ".0" or an exponent, so we re-add ".0" when needed.
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.14g", f);
            std::string s{buf};
            // If there's no '.', 'e', 'n', or 'i' (inf/nan handled above), it
            // printed as an integer-looking string; append ".0".
            bool has_decimal_or_exp =
                s.find('.') != std::string::npos ||
                s.find('e') != std::string::npos ||
                s.find('E') != std::string::npos ||
                s.find('n') != std::string::npos ||   // "nan"/"inf" (defensive)
                s.find('i') != std::string::npos;
            if (!has_decimal_or_exp) s += ".0";
            return s;
        }

        const char* type_name(const LuaValue& v) noexcept
        {
            switch (v.tag) {
            case LuaValue::Tag::Nil:     return "nil";
            case LuaValue::Tag::Bool:    return "boolean";
            case LuaValue::Tag::Int:
            case LuaValue::Tag::Flt:     return "number";
            case LuaValue::Tag::Str:     return "string";
            case LuaValue::Tag::Table:   return "table";
            case LuaValue::Tag::Closure:
            case LuaValue::Tag::Builtin: return "function";
            case LuaValue::Tag::Userdata:return "userdata";
            }
            return "unknown";
        }

        std::string value_to_string(const LuaValue& v)
        {
            switch (v.tag) {
            case LuaValue::Tag::Nil:  return "nil";
            case LuaValue::Tag::Bool: return v.as_bool() ? "true" : "false";
            case LuaValue::Tag::Int:
            case LuaValue::Tag::Flt:  return number_to_string(v);
            case LuaValue::Tag::Str:  return v.as_str()->data;
            case LuaValue::Tag::Table:
            case LuaValue::Tag::Closure:
            case LuaValue::Tag::Builtin:
            case LuaValue::Tag::Userdata: {
                // "table: 0x..." / "function: 0x..." — identity by address.
                std::ostringstream os;
                os << type_name(v) << ": " << static_cast<const void*>(v.as_gc());
                return os.str();
            }
            }
            return "<unknown>";
        }

        // -------------------------------------------------------------------
        // Error helper
        // -------------------------------------------------------------------
        [[noreturn]] static void type_err(const char* op, const LuaValue& v,
                                          std::size_t off)
        {
            throw LuaError(std::string("attempt to perform arithmetic on a ") +
                           type_name(v) + " value (local to '" + op + "')", off);
        }

        [[noreturn]] static void bit_err(const LuaValue& v, std::size_t off)
        {
            throw LuaError(std::string("number has no integer representation "
                                       "(bitwise op on a ") +
                           type_name(v) + " value)", off);
        }

        // Require a number; throw otherwise.
        static void require_number(const char* op, const LuaValue& v,
                                   std::size_t off)
        {
            if (!v.is_number()) type_err(op, v, off);
        }

        // -------------------------------------------------------------------
        // Arithmetic — int/float subtype rules
        // -------------------------------------------------------------------
        // Wrap a signed multiply/add/sub in unsigned arithmetic to get defined
        // two's-complement overflow (signed overflow is UB in C++).
        static long long wrap_u(unsigned long long x) noexcept
        {
            return static_cast<long long>(x);
        }

        LuaValue arith_add(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            require_number("+", a, off); require_number("+", b, off);
            if (either_is_float(a, b))
                return LuaValue::flt(to_double(a) + to_double(b));
            return LuaValue::integer(
                wrap_u(static_cast<unsigned long long>(a.as_int()) +
                       static_cast<unsigned long long>(b.as_int())));
        }

        LuaValue arith_sub(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            require_number("-", a, off); require_number("-", b, off);
            if (either_is_float(a, b))
                return LuaValue::flt(to_double(a) - to_double(b));
            return LuaValue::integer(
                wrap_u(static_cast<unsigned long long>(a.as_int()) -
                       static_cast<unsigned long long>(b.as_int())));
        }

        LuaValue arith_mul(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            require_number("*", a, off); require_number("*", b, off);
            if (either_is_float(a, b))
                return LuaValue::flt(to_double(a) * to_double(b));
            return LuaValue::integer(
                wrap_u(static_cast<unsigned long long>(a.as_int()) *
                       static_cast<unsigned long long>(b.as_int())));
        }

        LuaValue arith_div(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            require_number("/", a, off); require_number("/", b, off);
            // Division is ALWAYS a float in Lua, even int/int.
            return LuaValue::flt(to_double(a) / to_double(b));
        }

        // Floor division. int/int -> int (floor toward minus infinity); any
        // float -> float. Uses the identity floor(a/b); for the integer case
        // we use a division that floors (not truncates) to match Lua.
        LuaValue arith_idiv(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            require_number("//", a, off); require_number("//", b, off);
            if (either_is_float(a, b)) {
                double x = to_double(a), y = to_double(b);
                if (y == 0.0)
                    throw LuaError("attempt to perform 'n//0'", off);
                return LuaValue::flt(std::floor(x / y));
            }
            long long x = a.as_int(), y = b.as_int();
            if (y == 0)
                throw LuaError("attempt to perform 'n//0'", off);
            // Floor (not truncate) division for signed integers.
            long long q = x / y;
            long long r = x % y;
            if ((r != 0) && ((r < 0) != (y < 0))) --q;   // adjust toward -inf
            return LuaValue::integer(q);
        }

        LuaValue arith_mod(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            require_number("%", a, off); require_number("%", b, off);
            if (either_is_float(a, b)) {
                double x = to_double(a), y = to_double(b);
                if (y == 0.0)
                    throw LuaError("attempt to perform 'n%%0'", off);
                return LuaValue::flt(x - std::floor(x / y) * y);
            }
            long long x = a.as_int(), y = b.as_int();
            if (y == 0)
                throw LuaError("attempt to perform 'n%%0'", off);
            long long r = x % y;
            if ((r != 0) && ((r < 0) != (y < 0))) r += y;   // sign follows divisor
            return LuaValue::integer(r);
        }

        LuaValue arith_pow(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            require_number("^", a, off); require_number("^", b, off);
            // Power is ALWAYS a float in Lua.
            return LuaValue::flt(std::pow(to_double(a), to_double(b)));
        }

        // -------------------------------------------------------------------
        // Concatenation helpers (the evaluator owns the actual allocation)
        // -------------------------------------------------------------------
        bool concat_ok(const LuaValue& v) noexcept
        {
            return v.is_str() || v.is_number();
        }

        std::string concat_part(const LuaValue& v)
        {
            return v.is_str() ? v.as_str()->data : number_to_string(v);
        }

        // -------------------------------------------------------------------
        // Bitwise — integer-only. Floats (even integral-valued) error.
        // -------------------------------------------------------------------
        static void require_int(const LuaValue& v, std::size_t off)
        {
            if (!v.is_int()) bit_err(v, off);
        }

        // Lua shifts are on the full 64-bit value; SHR is logical.
        static long long shl(long long a, long long n) noexcept
        {
            if (n >= 63)      return 0;
            if (n <= -63)     return 0;
            if (n >= 0)       return wrap_u(static_cast<unsigned long long>(a) << n);
            n = -n;
            return wrap_u(static_cast<unsigned long long>(a) >> n);
        }
        static long long shr(long long a, long long n) noexcept
        {
            if (n >= 63)      return 0;
            if (n <= -63)     return 0;
            if (n >= 0)       return wrap_u(static_cast<unsigned long long>(a) >> n);
            n = -n;
            return wrap_u(static_cast<unsigned long long>(a) << n);
        }

        LuaValue arith_band(const LuaValue& a, const LuaValue& b, std::size_t off)
        { require_int(a, off); require_int(b, off);
          return LuaValue::integer(a.as_int() & b.as_int()); }
        LuaValue arith_bor (const LuaValue& a, const LuaValue& b, std::size_t off)
        { require_int(a, off); require_int(b, off);
          return LuaValue::integer(a.as_int() | b.as_int()); }
        LuaValue arith_bxor(const LuaValue& a, const LuaValue& b, std::size_t off)
        { require_int(a, off); require_int(b, off);
          return LuaValue::integer(a.as_int() ^ b.as_int()); }
        LuaValue arith_shl (const LuaValue& a, const LuaValue& b, std::size_t off)
        { require_int(a, off); require_int(b, off);
          return LuaValue::integer(shl(a.as_int(), b.as_int())); }
        LuaValue arith_shr (const LuaValue& a, const LuaValue& b, std::size_t off)
        { require_int(a, off); require_int(b, off);
          return LuaValue::integer(shr(a.as_int(), b.as_int())); }

        // -------------------------------------------------------------------
        // Unary
        // -------------------------------------------------------------------
        LuaValue arith_unm(const LuaValue& a, std::size_t off)
        {
            require_number("unary -", a, off);
            if (a.is_flt()) return LuaValue::flt(-a.as_flt());
            return LuaValue::integer(wrap_u(
                0ULL - static_cast<unsigned long long>(a.as_int())));
        }

        LuaValue arith_bnot(const LuaValue& a, std::size_t off)
        {
            require_int(a, off);
            return LuaValue::integer(~a.as_int());
        }

        // Length: strings (byte length) and tables (border). The table border
        // is computed lazily via table_border().
        LuaValue arith_len(const LuaValue& a, std::size_t off)
        {
            if (a.is_str())
                return LuaValue::integer(
                    static_cast<long long>(a.as_str()->data.size()));
            if (a.is_table())
                return LuaValue::integer(table_border(a.as_table()));
            throw LuaError(std::string("attempt to get length of a ") +
                           type_name(a) + " value", off);
        }

        // The border: scan up from 1 for the longest contiguous integer-keyed
        // prefix. O(n) in the array part; acceptable for M2.0 (a cache can be
        // added later, but invalidation on erase is fiddly and the honest scan
        // is simplest to verify).
        long long table_border(const Table* t) noexcept
        {
            LuaKey k;
            k.k = LuaKey::K::Int;
            long long n = 0;
            // Walk 1, 2, 3, ... until a missing slot.
            for (long long i = 1;; ++i) {
                k.i = i;
                auto it = t->hash.find(k);
                if (it == t->hash.end() || it->second.is_nil()) break;
                n = i;
            }
            return n;
        }

        LuaValue arith_not(const LuaValue& a) noexcept
        {
            return LuaValue::boolean(!a.truthy());
        }

        // -------------------------------------------------------------------
        // Equality + ordering
        // -------------------------------------------------------------------
        bool raw_equal(const LuaValue& a, const LuaValue& b) noexcept
        {
            // Numbers compare by value across subtypes: 1 == 1.0.
            if (a.is_number() && b.is_number())
                return to_double(a) == to_double(b);
            if (a.tag != b.tag) return false;
            switch (a.tag) {
            case LuaValue::Tag::Nil:     return true;
            case LuaValue::Tag::Bool:    return a.as_bool() == b.as_bool();
            case LuaValue::Tag::Str:     return a.as_str()->data == b.as_str()->data;
            case LuaValue::Tag::Table:   return a.as_table() == b.as_table();
            case LuaValue::Tag::Closure: return a.as_closure() == b.as_closure();
            case LuaValue::Tag::Builtin: return a.as_builtin() == b.as_builtin();
            case LuaValue::Tag::Int:
            case LuaValue::Tag::Flt:     return false;   // handled above
            }
            return false;
        }

        int raw_cmp(const LuaValue& a, const LuaValue& b, std::size_t off)
        {
            if (a.is_number() && b.is_number()) {
                double x = to_double(a), y = to_double(b);
                if (x < y) return -1;
                if (x > y) return 1;
                return 0;
            }
            if (a.is_str() && b.is_str()) {
                const auto& x = a.as_str()->data;
                const auto& y = b.as_str()->data;
                int c = std::memcmp(x.data(), y.data(),
                                    std::min(x.size(), y.size()));
                if (c != 0) return c < 0 ? -1 : 1;
                if (x.size() < y.size()) return -1;
                if (x.size() > y.size()) return 1;
                return 0;
            }
            throw LuaError(std::string("attempt to compare ") + type_name(a) +
                           " with " + type_name(b), off);
        }

        bool for_num(const LuaValue& v, double& out) noexcept
        {
            if (!v.is_number()) return false;
            out = to_double(v);
            return true;
        }

    } // namespace lua
} // namespace ys
