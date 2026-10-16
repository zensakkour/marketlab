#pragma once

#include <marketlab/quantity.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace marketlab {

// Exact nonnegative units: high_units * 2^64 + low_units.
// Every pair of words is valid; matching still uses the bounded Quantity type.
struct AggregateQuantity {
    std::uint64_t high_units{};
    std::uint64_t low_units{};

    [[nodiscard]] constexpr std::optional<AggregateQuantity> add(Quantity quantity) const noexcept {
        const auto low = low_units + static_cast<std::uint64_t>(quantity.units());
        const auto carry = static_cast<std::uint64_t>(low < low_units);
        if (carry != 0 && high_units == std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
        return AggregateQuantity{high_units + carry, low};
    }

    constexpr auto operator<=>(const AggregateQuantity&) const noexcept = default;
};

[[nodiscard]] inline std::string to_string(AggregateQuantity quantity) {
    constexpr std::uint64_t mask = 0xffffffff;
    std::array<std::uint64_t, 4> words{
        quantity.high_units >> 32, quantity.high_units & mask,
        quantity.low_units >> 32, quantity.low_units & mask};
    // UINT128_MAX has 39 decimal digits. Divide base-2^32 words to avoid wider arithmetic.
    std::array<char, 39> digits{};
    auto position = digits.size();
    bool nonzero;
    do {
        std::uint64_t remainder = 0;
        nonzero = false;
        for (auto& word : words) {
            const auto dividend = (remainder << 32) | word;
            word = dividend / 10;
            remainder = dividend % 10;
            nonzero = nonzero || word != 0;
        }
        digits[--position] = static_cast<char>('0' + remainder);
    } while (nonzero);
    return {digits.data() + position, digits.size() - position};
}

} // namespace marketlab
