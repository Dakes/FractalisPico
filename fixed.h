#ifndef FIXED_H
#define FIXED_H

#include <cmath>
#include <cstdint>

/**
 * Signed fixed point number for coordinates and the reference orbit: 256 bit in two's complement, 8 integer bits
 * (incl. sign, range +-128) and 248 fraction bits (~74 decimal digits). Addition is exact, multiplication only
 * needs integer instructions.
 */
#ifndef FIXED_LIMBS
#define FIXED_LIMBS 8
#endif

struct Fixed {
    static constexpr int LIMBS = FIXED_LIMBS;
    static constexpr int INTEGER_BITS = 8;
    static constexpr int FRACTION_BITS = 32 * LIMBS - INTEGER_BITS;

    uint32_t limb[LIMBS];  // least significant first

    Fixed() = default;
    // Exact (bits below the precision are cut off), saturates outside of the range
    Fixed(double value) {
        for (int i = 0; i < LIMBS; ++i) limb[i] = 0;
        if (!(value != 0) || std::isnan(value)) return;
        bool negative = value < 0;
        int exponent;
        double mantissa = std::frexp(std::fabs(value), &exponent);  // [0.5, 1)
        uint64_t bits = static_cast<uint64_t>(std::ldexp(mantissa, 53));
        // value = bits * 2^(exponent - 53), the fixed point integer is value * 2^FRACTION_BITS
        int shift = exponent - 53 + FRACTION_BITS;
        if (shift + 53 > 32 * LIMBS - 1) {
            for (int i = 0; i < LIMBS - 1; ++i) limb[i] = 0xFFFFFFFF;
            limb[LIMBS - 1] = 0x7FFFFFFF;
        } else if (shift < 0) {
            if (shift > -64) {
                bits >>= -shift;
                limb[0] = static_cast<uint32_t>(bits);
                limb[1] = static_cast<uint32_t>(bits >> 32);
            }
        } else {
            // bits << shift spans 3 limbs
            int index = shift / 32, offset = shift % 32;
            uint64_t low = bits << offset;
            uint32_t high = offset ? static_cast<uint32_t>(bits >> (64 - offset)) : 0;
            limb[index] = static_cast<uint32_t>(low);
            if (index + 1 < LIMBS) limb[index + 1] = static_cast<uint32_t>(low >> 32);
            if (index + 2 < LIMBS) limb[index + 2] = high;
        }
        if (negative) *this = -*this;
    }

    bool negative() const { return limb[LIMBS - 1] >> 31; }

    double to_double() const {
        Fixed a = negative() ? -*this : *this;
        int i = LIMBS - 1;
        while (i >= 0 && a.limb[i] == 0) --i;
        if (i < 0) return 0.0;
        // The top 3 limbs from the first one that isn't 0
        double r = a.limb[i];
        int lowest = i;
        for (int k = 1; k <= 2 && i - k >= 0; ++k) {
            r = r * 4294967296.0 + a.limb[i - k];
            lowest = i - k;
        }
        r = std::ldexp(r, 32 * lowest - FRACTION_BITS);
        return negative() ? -r : r;
    }

    Fixed operator-() const {
        Fixed r;
        uint32_t carry = 1;
        for (int i = 0; i < LIMBS; ++i) {
            uint32_t v = ~limb[i] + carry;
            carry = carry && v == 0;
            r.limb[i] = v;
        }
        return r;
    }

    Fixed operator+(const Fixed& b) const {
        Fixed r;
        uint64_t carry = 0;
        for (int i = 0; i < LIMBS; ++i) {
            carry += static_cast<uint64_t>(limb[i]) + b.limb[i];
            r.limb[i] = static_cast<uint32_t>(carry);
            carry >>= 32;
        }
        return r;
    }

    Fixed operator-(const Fixed& b) const {
        Fixed r;
        int64_t borrow = 0;
        for (int i = 0; i < LIMBS; ++i) {
            int64_t v = static_cast<int64_t>(limb[i]) - b.limb[i] + borrow;
            r.limb[i] = static_cast<uint32_t>(v);
            borrow = v >> 32;
        }
        return r;
    }

    // From a decimal number like "-0.7436...", exact up to the precision
    static Fixed parse(const char* text) {
        bool neg = *text == '-';
        if (*text == '-' || *text == '+') ++text;
        Fixed value = 0.0, place = 1.0;
        bool fraction = false;
        for (; *text; ++text) {
            if (*text == '.') {
                fraction = true;
            } else if (*text >= '0' && *text <= '9') {
                int digit = *text - '0';
                if (fraction) {
                    place = place.divided(10);
                    value += place.times(digit);
                } else {
                    value = value.times(10) + Fixed(static_cast<double>(digit));
                }
            }
        }
        return neg ? -value : value;
    }

    // Division by a small positive integer, rounded towards 0
    Fixed divided(uint32_t divisor) const {
        Fixed a = negative() ? -*this : *this;
        uint64_t remainder = 0;
        for (int i = LIMBS - 1; i >= 0; --i) {
            uint64_t current = (remainder << 32) | a.limb[i];
            a.limb[i] = static_cast<uint32_t>(current / divisor);
            remainder = current % divisor;
        }
        return negative() ? -a : a;
    }

    Fixed& operator+=(const Fixed& b) { return *this = *this + b; }
    Fixed& operator-=(const Fixed& b) { return *this = *this - b; }

    Fixed twice() const {
        Fixed r;
        for (int i = LIMBS - 1; i > 0; --i) r.limb[i] = (limb[i] << 1) | (limb[i - 1] >> 31);
        r.limb[0] = limb[0] << 1;
        return r;
    }

    // Exact multiplication with a small integer
    Fixed times(int factor) const {
        bool neg = (factor < 0) != negative();
        Fixed a = negative() ? -*this : *this;
        uint64_t f = static_cast<uint64_t>(factor < 0 ? -static_cast<int64_t>(factor) : factor);
        Fixed r;
        uint64_t carry = 0;
        for (int i = 0; i < LIMBS; ++i) {
            carry += a.limb[i] * f;
            r.limb[i] = static_cast<uint32_t>(carry);
            carry >>= 32;
        }
        return neg ? -r : r;
    }

    /**
     * Product, rounded towards 0. Only the partial products that reach the result bits are calculated (the ones
     * below only add carries in the order of 2^-50 of the last bit).
     */
    friend Fixed operator*(const Fixed& x, const Fixed& y) {
        bool neg = x.negative() != y.negative();
        Fixed a = x.negative() ? -x : x;
        Fixed b = y.negative() ? -y : y;
        // Columns of the 2 * LIMBS limb product, the result starts at bit FRACTION_BITS = 32 * (LIMBS - 1) + 24
        uint32_t column[2 * LIMBS + 1];
        uint64_t low = 0;
        uint32_t high = 0;
        for (int k = LIMBS - 2; k <= 2 * LIMBS - 2; ++k) {
            int first = k - (LIMBS - 1) > 0 ? k - (LIMBS - 1) : 0;
            int last = k < LIMBS - 1 ? k : LIMBS - 1;
            for (int i = first; i <= last; ++i) {
                uint64_t p = static_cast<uint64_t>(a.limb[i]) * b.limb[k - i];
                low += p;
                high += low < p;
            }
            column[k] = static_cast<uint32_t>(low);
            low = (low >> 32) | (static_cast<uint64_t>(high) << 32);
            high = 0;
        }
        column[2 * LIMBS - 1] = static_cast<uint32_t>(low);
        column[2 * LIMBS] = 0;
        constexpr int SHIFT = FRACTION_BITS - 32 * (LIMBS - 1);
        Fixed r;
        for (int j = 0; j < LIMBS; ++j) {
            r.limb[j] = (column[LIMBS - 1 + j] >> SHIFT) | (column[LIMBS + j] << (32 - SHIFT));
        }
        return neg ? -r : r;
    }
};

#endif // FIXED_H
