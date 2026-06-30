#pragma once

// Number-aware arithmetic primitives for the evaluator, implementing Lua 5.4's
// integer/float subtype semantics. These are pure functions over LuaValue (no
// Heap, no Environment) so they are trivially testable in isolation.
//
// Rules (reference Lua 5.4 §3.4.1–3.4.3):
//   + - *   : int+int -> int (wrap in unsigned long long, two's-complement);
//             any float -> float
//   /       : always float
//   //      : floor division — int/int -> int, any float -> float
//   %       : a - floor(a/b)*b, same subtype rules as //
//   ^       : always float (Lua power is always a float)
//   & | ~ << >> : integer-only; error if either operand is a float
//   ..      : number operands are converted to their string form first
//   unary - : preserve subtype
//   unary ~ : integer-only
//
// Comparisons:
//   ==      : numbers compare by value across subtypes (1 == 1.0 is true);
//             different non-number types are unequal; strings by content;
//             objects by identity
//   < <= > >=: numbers (cross-subtype) or two strings (lexicographic); else error

#include <string>

#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        // The numeric subset of a value as a double (for mixed-mode ops).
        double to_double(const LuaValue& v) noexcept;

        // Does the value represent a (possibly integral) float, i.e. is it a
        // float? Used to decide int-vs-float result subtype.
        inline bool either_is_float(const LuaValue& a, const LuaValue& b) noexcept
        {
            return a.is_flt() || b.is_flt();
        }

        // Render a number value as Lua would. Integers use plain decimal;
        // floats use %.14g with a guarantee that a float always shows a decimal
        // point or exponent (so 3.0 is "3.0", distinct from int 3's "3"), and
        // inf/-inf/nan render as "inf"/"-inf"/"-nan" / "nan".
        std::string number_to_string(const LuaValue& v);

        // Render ANY value as a string (for tostring / .. / print). Numbers use
        // number_to_string; strings are themselves; nil/bool use their names;
        // objects use "table: 0x.."/"function: 0x..".
        std::string value_to_string(const LuaValue& v);

        // The Lua type name of a value, as type() returns it.
        const char* type_name(const LuaValue& v) noexcept;

        // True if both are numbers (int or float). Used by comparison helpers.
        inline bool both_numbers(const LuaValue& a, const LuaValue& b) noexcept
        {
            return a.is_number() && b.is_number();
        }

        // Core arithmetic, each throwing LuaError on a type error (non-number
        // operand, or float in a bitwise op). The `off` is attached to errors.
        LuaValue arith_add(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_sub(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_mul(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_div(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_idiv(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_mod(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_pow(const LuaValue& a, const LuaValue& b, std::size_t off);
        // Concat is handled by the evaluator (it needs the Heap to allocate the
        // result string); these two helpers validate operands and render them.
        bool      concat_ok(const LuaValue& v) noexcept;   // string or number
        std::string concat_part(const LuaValue& v);         // render for ..

        // Bitwise — integer-only; error if a float operand appears.
        LuaValue arith_band(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_bor (const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_bxor(const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_shl (const LuaValue& a, const LuaValue& b, std::size_t off);
        LuaValue arith_shr (const LuaValue& a, const LuaValue& b, std::size_t off);

        // Unary. Neg preserves subtype; bnot is integer-only.
        LuaValue arith_unm(const LuaValue& a, std::size_t off);
        LuaValue arith_bnot(const LuaValue& a, std::size_t off);
        LuaValue arith_len(const LuaValue& a, std::size_t off);   // # : string bytes / table border
        // The border of a table: largest n with t[n]~=nil and t[n+1]==nil
        // (Lua §3.4.7). Pure function over the table's hash map.
        long long table_border(const Table* t) noexcept;
        LuaValue arith_not(const LuaValue& a) noexcept;           // logical, never throws

        // Equality across all types (the `==` operator, no metamethods in M2.0).
        bool raw_equal(const LuaValue& a, const LuaValue& b) noexcept;

        // Ordering. Returns -1/0/1 (a<b/a==b/a>b) or throws on incomparable
        // types. Used by < <= > >=.
        int raw_cmp(const LuaValue& a, const LuaValue& b, std::size_t off);

        // A check: is this value a valid <limit> for a numeric for? (Must be
        // coercible to a number; Lua would try string->number, but M2.0 only
        // accepts actual numbers.) Returns true and sets out for numbers.
        bool for_num(const LuaValue& v, double& out) noexcept;

    } // namespace lua
} // namespace ys
