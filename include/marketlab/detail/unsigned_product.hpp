#pragma once

#include <cstdint>

namespace marketlab::detail {

struct UnsignedProduct {
    std::uint64_t high;
    std::uint64_t low;
    constexpr bool operator==(const UnsignedProduct&) const noexcept = default;
};

// Full 64-by-64 product using only standard uint64 arithmetic.
[[nodiscard]] constexpr UnsignedProduct multiply_unsigned64(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr std::uint64_t mask = 0xffffffff;
    const auto lower = (left & mask) * (right & mask);
    const auto cross_left = (left >> 32) * (right & mask);
    const auto cross_right = (left & mask) * (right >> 32);
    // Three 32-bit terms fit in 34 bits. Retain their carry into the high word.
    const auto middle = (lower >> 32) + (cross_left & mask) + (cross_right & mask);
    const auto high = (left >> 32) * (right >> 32) + (cross_left >> 32) +
        (cross_right >> 32) + (middle >> 32);
    // Unsigned truncation of middle << 32 deliberately keeps its low 32 bits.
    return {high, (middle << 32) | (lower & mask)};
}

} // namespace marketlab::detail
