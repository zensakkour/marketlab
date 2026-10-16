#include <marketlab/cash_amount.hpp>

#include "quantity_oracle.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

using marketlab::CashAmount;
using marketlab::Price;
using marketlab::Quantity;

constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
constexpr CashAmount one{0, 0, 1};
constexpr CashAmount full{maximum, maximum, maximum};
static_assert(CashAmount{}.add(one) == one);
static_assert(CashAmount{0, 0, maximum}.add(one) == CashAmount{0, 1, 0});
static_assert(CashAmount{0, maximum, maximum}.add(one) == CashAmount{1, 0, 0});
static_assert(CashAmount{0, maximum, 0}.add({0, 1, 0}) == CashAmount{1, 0, 0});
static_assert(!full.add(one));
static_assert(!CashAmount{maximum, 0, 0}.add({1, 0, 0}));
static_assert(full.add({}) == full);
static_assert(CashAmount{1, 0, 0}.subtract(one) == CashAmount{0, maximum, maximum});
static_assert(CashAmount{1, 0, 0}.subtract({0, maximum, 1}) == CashAmount{0, 0, maximum});
static_assert(CashAmount{1, 0, 0}.subtract({0, 0, maximum}) == CashAmount{0, maximum, 1});
static_assert(full.subtract(full) == CashAmount{});
static_assert(!CashAmount{}.subtract(one));
static_assert(noexcept(one.add(full)) && noexcept(full.subtract(one)));
static_assert(CashAmount{}.multiply(maximum) == CashAmount{});
static_assert(full.multiply(0) == CashAmount{});
static_assert(full.multiply(1) == full);
static_assert(CashAmount{0, 0, maximum}.multiply(maximum) == CashAmount{0, maximum - 1, 1});
static_assert(CashAmount{0, maximum, maximum}.multiply(2) == CashAmount{1, maximum, maximum - 1});
static_assert(CashAmount{0, maximum, maximum}.multiply(maximum) == CashAmount{maximum - 1, maximum, 1});
static_assert(!CashAmount{std::uint64_t{1} << 63, 0, 0}.multiply(2));
static_assert(!CashAmount{maximum / 3, maximum, 0}.multiply(3));
static_assert(!CashAmount{maximum / 3, maximum / 3, maximum}.multiply(3));
static_assert(noexcept(full.multiply(maximum)));
static_assert(!std::is_convertible_v<CashAmount, marketlab::Quantity>);
static_assert(CashAmount::from_trade(Price::from_ticks(3).value(), Quantity::from_units(5).value()) == CashAmount{0, 0, 15});

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

void test_boundaries() {
    struct Case { CashAmount value; std::string_view decimal; };
    const std::array cases{
        Case{{}, "0"}, Case{one, "1"}, Case{{0, 0, maximum}, "18446744073709551615"},
        Case{{0, 1, 0}, "18446744073709551616"},
        Case{{1, 0, 0}, "340282366920938463463374607431768211456"},
        Case{full, "6277101735386680763835789423207666416102355444464034512895"}
    };
    for (const auto& test : cases) require(marketlab::to_string(test.value) == test.decimal, "cash decimal boundary differs");
    constexpr auto bound = std::numeric_limits<std::int64_t>::max();
    const auto largest_trade = CashAmount::from_trade(Price::from_ticks(bound).value(), Quantity::from_units(bound).value());
    require(largest_trade == CashAmount{0, 0x3fffffffffffffff, 1}, "maximum trade product differs");
    CashAmount total{};
    for (int index = 0; index < 5; ++index) total = total.add(largest_trade).value();
    require(total == CashAmount{1, 0x3ffffffffffffffb, 5} && marketlab::to_string(total) == "425352958651173079236984538921162506245",
        "five maximum trade notionals wrapped at 128 bits");
    require(CashAmount::from_trade(Price::from_ticks(bound).value(), Quantity::from_units(0).value()) == CashAmount{},
        "zero quantity notional differs");
}

void test_generated_totals() {
    std::mt19937_64 random{42};
    CashAmount total{};
    std::string decimal{"0"};
    for (int index = 0; index < 10000; ++index) {
        const auto ticks = static_cast<std::int64_t>((random() >> 1) | 1);
        const auto units = static_cast<std::int64_t>(random() >> 1);
        const auto amount = CashAmount::from_trade(Price::from_ticks(ticks).value(), Quantity::from_units(units).value());
        const auto expected = decimal_product(static_cast<std::uint64_t>(ticks), static_cast<std::uint64_t>(units));
        require(marketlab::to_string(amount) == expected, "trade notional differs from decimal product");
        const auto next = total.add(amount);
        require(next.has_value() && next->subtract(total) == amount && next->subtract(amount) == total,
            "cash addition/subtraction round trip differs");
        total = *next;
        decimal = add_decimal(decimal, expected);
        require(marketlab::to_string(total) == decimal, "cash total differs from independent decimal sum");
    }
}

void test_scalar_products() {
    std::mt19937_64 random{2026};
    const auto maximum_decimal = marketlab::to_string(full);
    std::size_t fitting = 0;
    std::size_t overflowing = 0;
    for (int index = 0; index < 10000; ++index) {
        // Mix full-width overflow with fitting 128-by-64 and 192-by-small products.
        const CashAmount amount{index % 3 == 0 ? 0 : random(), random(), random()};
        const auto factor = index % 3 == 2 ? random() % 4 : random();
        const auto decimal = decimal_product(marketlab::to_string(amount), std::to_string(factor));
        const bool overflow = decimal.size() > maximum_decimal.size() ||
            (decimal.size() == maximum_decimal.size() && decimal > maximum_decimal);
        const auto saved = amount;
        const auto product = amount.multiply(factor);
        require(amount == saved && product.has_value() != overflow, "cash scalar overflow or source differs");
        if (product) {
            ++fitting;
            require(marketlab::to_string(*product) == decimal, "cash scalar product differs from decimal oracle");
        } else {
            ++overflowing;
        }
    }
    require(fitting > 3000 && overflowing > 3000, "cash scalar workload missed fitting or overflow cases");
}

int main() {
    try {
        test_boundaries();
        test_generated_totals();
        test_scalar_products();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
