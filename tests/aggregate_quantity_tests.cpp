#include <marketlab/aggregate_quantity.hpp>

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
#include <utility>

using marketlab::AggregateQuantity;
using marketlab::Quantity;

constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
constexpr auto one = Quantity::from_units(1).value();
constexpr auto zero = Quantity::from_units(0).value();
static_assert(AggregateQuantity{}.add(zero) == AggregateQuantity{});
static_assert(AggregateQuantity{0, maximum}.add(one) == AggregateQuantity{1, 0});
static_assert(AggregateQuantity{maximum, maximum - 1}.add(one) == AggregateQuantity{maximum, maximum});
static_assert(!AggregateQuantity{maximum, maximum}.add(one));
static_assert(AggregateQuantity{maximum, maximum}.add(zero) == AggregateQuantity{maximum, maximum});
static_assert(AggregateQuantity{1, 0} > AggregateQuantity{0, maximum});
static_assert(noexcept(AggregateQuantity{}.add(one)));
static_assert(AggregateQuantity{}.subtract({}) == AggregateQuantity{});
static_assert(!AggregateQuantity{}.subtract({0, 1}));
static_assert(AggregateQuantity{1, 0}.subtract({0, 1}) == AggregateQuantity{0, maximum});
static_assert(AggregateQuantity{maximum, 0}.subtract({maximum - 1, maximum}) == AggregateQuantity{0, 1});
static_assert(AggregateQuantity{maximum, maximum}.subtract({maximum, maximum}) == AggregateQuantity{});
static_assert(AggregateQuantity{maximum, maximum}.subtract({}) == AggregateQuantity{maximum, maximum});
static_assert(!AggregateQuantity{1, maximum}.subtract({2, 0}));
static_assert(noexcept(AggregateQuantity{}.subtract({})));

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

void test_decimal_boundaries() {
    struct Case { AggregateQuantity value; std::string_view decimal; };
    const std::array cases{
        Case{{}, "0"}, Case{{0, 1}, "1"}, Case{{0, 10}, "10"},
        Case{{0, maximum}, "18446744073709551615"},
        Case{{1, 0}, "18446744073709551616"},
        Case{{1, 1}, "18446744073709551617"},
        Case{{0x8000000000000000, 0}, "170141183460469231731687303715884105728"},
        Case{{maximum, maximum}, "340282366920938463463374607431768211455"}
    };
    for (const auto& test : cases) {
        require(marketlab::to_string(test.value) == test.decimal, "exact aggregate decimal differs");
    }
    const AggregateQuantity full{maximum, maximum};
    require(!full.add(one) && full.high_units == maximum && full.low_units == maximum,
        "aggregate overflow changed the source value");
}

void test_generated_additions() {
    std::mt19937_64 random{42};
    AggregateQuantity total{};
    std::string decimal{"0"};
    constexpr auto maximum_units = std::numeric_limits<std::int64_t>::max();
    for (int index = 0; index < 10000; ++index) {
        const auto units = index % 3 == 0 ? maximum_units :
            static_cast<std::int64_t>(random() >> 1);
        const auto next = total.add(Quantity::from_units(units).value());
        require(next.has_value(), "bounded aggregate sum overflowed");
        const AggregateQuantity added{0, static_cast<std::uint64_t>(units)};
        require(next->subtract(total) == added && next->subtract(added) == total,
            "aggregate subtraction failed to recover the generated operands");
        total = *next;
        decimal = add_decimal_units(std::move(decimal), static_cast<std::uint64_t>(units));
        require(marketlab::to_string(total) == decimal, "generated aggregate differs from decimal oracle");
    }
}

int main() {
    try {
        test_decimal_boundaries();
        test_generated_additions();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
