module;
#include <cstdint>

module anndatapp.core;
import :arrays;

namespace anndatapp {
Float16 Float16::from_double(double value) noexcept {
    const auto u = __builtin_bit_cast(std::uint64_t, value);
    const auto sign = std::uint16_t((u >> 48) & 0x8000);
    const auto exponent = unsigned((u >> 52) & 0x7ff);
    const auto fraction = u & ((std::uint64_t{1} << 52) - 1);
    if (exponent == 0x7ff) return {std::uint16_t(fraction ? 0x7e00 : sign | 0x7c00)};
    const int e = int(exponent) - 1023;
    if (e > 15) return {std::uint16_t(sign | 0x7c00)};
    if (e < -25) return {sign};
    const auto mantissa = (std::uint64_t{1} << 52) | fraction;
    const int shift = e >= -14 ? 42 : 28 - e;
    auto rounded = mantissa >> shift;
    const auto remainder = mantissa & ((std::uint64_t{1} << shift) - 1);
    const auto halfway = std::uint64_t{1} << (shift - 1);
    rounded += remainder > halfway || (remainder == halfway && (rounded & 1));
    if (e < -14) return {std::uint16_t(sign | rounded)};
    const auto encoded = (std::uint64_t(e + 15) << 10) + (rounded - 1024);
    return {std::uint16_t(sign | encoded)};
}
Float16 Float16::from_float(float value) noexcept { return from_double(double(value)); }
float Float16::to_float() const noexcept {
    const auto sign = std::uint32_t(bits & 0x8000) << 16;
    const auto exponent = unsigned((bits >> 10) & 0x1f);
    auto fraction = std::uint32_t(bits & 0x3ff);
    if (exponent == 0x1f)
        return __builtin_bit_cast(float, fraction ? 0x7fc00000u : sign | 0x7f800000u);
    if (!exponent) {
        if (!fraction) return __builtin_bit_cast(float, sign);
        int e = -14;
        while (!(fraction & 0x400)) { fraction <<= 1; --e; }
        return __builtin_bit_cast(float, sign | (std::uint32_t(e + 127) << 23) | ((fraction & 0x3ff) << 13));
    }
    return __builtin_bit_cast(float, sign | (std::uint32_t(exponent + 112) << 23) | (fraction << 13));
}
double Float16::to_double() const noexcept { return double(to_float()); }
} // namespace anndatapp
