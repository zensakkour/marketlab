#pragma once

#include <marketlab/aggregate_quantity.hpp>
#include <marketlab/detail/unsigned_product.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>

namespace marketlab {

// Exact nonnegative tick-times-quantity units, high word first.
struct CashAmount {
    std::uint64_t high_units{};
    std::uint64_t middle_units{};
    std::uint64_t low_units{};

    [[nodiscard]] static constexpr CashAmount from_trade(Price price, Quantity quantity) noexcept {
        const auto product = detail::multiply_unsigned64(
            static_cast<std::uint64_t>(price.ticks()), static_cast<std::uint64_t>(quantity.units()));
        return {0, product.high, product.low};
    }

    [[nodiscard]] static constexpr CashAmount from_aggregate(Price price, AggregateQuantity quantity) noexcept {
        // A 128-bit quantity times a positive int64 price is always below 2^191.
        return *CashAmount{0, quantity.high_units, quantity.low_units}.multiply(
            static_cast<std::uint64_t>(price.ticks()));
    }

    [[nodiscard]] constexpr std::optional<CashAmount> add(CashAmount other) const noexcept {
        const auto low = low_units + other.low_units;
        const auto low_carry = static_cast<std::uint64_t>(low < low_units);
        const auto middle = middle_units + other.middle_units;
        const auto middle_with_carry = middle + low_carry;
        const auto high_carry = static_cast<std::uint64_t>(
            middle < middle_units || middle_with_carry < middle);
        const auto high = high_units + other.high_units;
        const auto high_with_carry = high + high_carry;
        if (high < high_units || high_with_carry < high) {
            return std::nullopt;
        }
        return CashAmount{high_with_carry, middle_with_carry, low};
    }

    [[nodiscard]] constexpr std::optional<CashAmount> multiply(std::uint64_t factor) const noexcept {
        const auto low = detail::multiply_unsigned64(low_units, factor);
        const auto middle = detail::multiply_unsigned64(middle_units, factor);
        const auto high = detail::multiply_unsigned64(high_units, factor);
        const auto middle_sum = middle.low + low.high;
        const auto carry = static_cast<std::uint64_t>(middle_sum < middle.low);
        const auto high_sum = high.low + middle.high;
        const auto high_with_carry = high_sum + carry;
        if (high.high != 0 || high_sum < high.low || high_with_carry < high_sum) {
            return std::nullopt;
        }
        return CashAmount{high_with_carry, middle_sum, low.low};
    }

    [[nodiscard]] constexpr std::optional<CashAmount> subtract(CashAmount other) const noexcept {
        if (other > *this) {
            return std::nullopt;
        }
        const auto low_borrow = static_cast<std::uint64_t>(low_units < other.low_units);
        const auto middle = middle_units - other.middle_units;
        const auto high_borrow = static_cast<std::uint64_t>(
            middle_units < other.middle_units || middle < low_borrow);
        return CashAmount{high_units - other.high_units - high_borrow,
            middle - low_borrow, low_units - other.low_units};
    }

    constexpr auto operator<=>(const CashAmount&) const noexcept = default;
};

struct NetCashFlow {
    bool negative{};
    CashAmount magnitude{};
    constexpr bool operator==(const NetCashFlow&) const noexcept = default;
};

[[nodiscard]] inline std::string to_string(CashAmount amount) {
    constexpr std::uint64_t mask = 0xffffffff;
    std::array<std::uint64_t, 6> words{
        amount.high_units >> 32, amount.high_units & mask,
        amount.middle_units >> 32, amount.middle_units & mask,
        amount.low_units >> 32, amount.low_units & mask};
    // UINT192_MAX has 58 digits; base-2^32 division needs no wider arithmetic.
    std::array<char, 58> digits{};
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
