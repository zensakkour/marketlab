#include <marketlab/identifiers.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/side.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <type_traits>

using marketlab::InstrumentId;
using marketlab::OrderId;
using marketlab::ParticipantId;
using marketlab::Quantity;
using marketlab::Side;
using marketlab::is_valid;
using marketlab::is_valid_order_quantity;

static_assert(!std::is_default_constructible_v<Quantity>);
static_assert(!std::is_convertible_v<std::int64_t, Quantity>);
static_assert(!std::is_convertible_v<OrderId, ParticipantId>);
static_assert(!std::is_convertible_v<ParticipantId, InstrumentId>);
static_assert(!std::is_convertible_v<InstrumentId, OrderId>);
static_assert(!std::is_convertible_v<std::uint64_t, OrderId>);
static_assert(!std::is_convertible_v<std::uint64_t, ParticipantId>);
static_assert(!std::is_convertible_v<std::uint64_t, InstrumentId>);
static_assert(!is_valid(OrderId{}));
static_assert(!is_valid(ParticipantId{}));
static_assert(!is_valid(InstrumentId{}));
constexpr auto zero = Quantity::from_units(0).value();
constexpr auto five = Quantity::from_units(5).value();
static_assert(!is_valid_order_quantity(zero));
static_assert(is_valid_order_quantity(five));
static_assert(five.subtract(five)->units() == 0);
static_assert(!zero.subtract(five));

int main() {
    struct QuantityCase {
        std::int64_t units;
        bool accepted;
        bool valid_order_quantity;
    };
    constexpr std::array quantity_cases{
        QuantityCase{std::numeric_limits<std::int64_t>::min(), false, false},
        QuantityCase{-1, false, false},
        QuantityCase{0, true, false},
        QuantityCase{1, true, true},
        QuantityCase{5, true, true},
        QuantityCase{std::numeric_limits<std::int64_t>::max(), true, true},
    };
    for (const auto& test : quantity_cases) {
        const auto quantity = Quantity::from_units(test.units);
        if (quantity.has_value() != test.accepted ||
            (quantity && (quantity->units() != test.units ||
                          is_valid_order_quantity(*quantity) != test.valid_order_quantity))) {
            std::cerr << "unexpected quantity validation for " << test.units << '\n';
            return 1;
        }
    }

    struct SubtractionCase {
        std::int64_t remaining;
        std::int64_t removed;
        std::optional<std::int64_t> expected;
    };
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr std::array subtraction_cases{
        SubtractionCase{5, 2, 3},
        SubtractionCase{5, 5, 0},
        SubtractionCase{5, 0, 5},
        SubtractionCase{0, 0, 0},
        SubtractionCase{2, 5, std::nullopt},
        SubtractionCase{0, maximum, std::nullopt},
        SubtractionCase{maximum, maximum, 0},
        SubtractionCase{maximum, 1, maximum - 1},
    };
    for (const auto& test : subtraction_cases) {
        const auto remaining = Quantity::from_units(test.remaining).value();
        const auto removed = Quantity::from_units(test.removed).value();
        const auto result = remaining.subtract(removed);
        if (result.has_value() != test.expected.has_value() ||
            (result && result->units() != test.expected.value())) {
            std::cerr << "unexpected subtraction result for " << test.remaining << " - "
                      << test.removed << '\n';
            return 1;
        }
        if (remaining.units() != test.remaining || removed.units() != test.removed) {
            std::cerr << "subtraction changed its inputs\n";
            return 1;
        }
    }
    if (!(zero < five) || five != Quantity::from_units(5).value()) {
        std::cerr << "unexpected quantity comparison\n";
        return 1;
    }

    struct IdCase {
        std::uint64_t value;
        bool accepted;
    };
    constexpr std::array id_cases{
        IdCase{0, false},
        IdCase{1, true},
        IdCase{std::numeric_limits<std::uint64_t>::max(), true},
    };
    for (const auto& test : id_cases) {
        if (is_valid(OrderId{test.value}) != test.accepted ||
            is_valid(ParticipantId{test.value}) != test.accepted ||
            is_valid(InstrumentId{test.value}) != test.accepted) {
            std::cerr << "unexpected identifier validation for " << test.value << '\n';
            return 1;
        }
    }

    if (!is_valid(Side::Buy) || !is_valid(Side::Sell) ||
        is_valid(static_cast<Side>(0)) || is_valid(static_cast<Side>(3)) ||
        is_valid(static_cast<Side>(std::numeric_limits<std::uint8_t>::max()))) {
        std::cerr << "unexpected side validation\n";
        return 1;
    }
}
