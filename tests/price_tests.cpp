#include <marketlab/price.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>

using marketlab::Price;

static_assert(!std::is_default_constructible_v<Price>);
static_assert(!std::is_convertible_v<std::int64_t, Price>);
constexpr auto compile_time_price = Price::from_ticks(10'001);
static_assert(compile_time_price && compile_time_price->ticks() == 10'001);
static_assert(!Price::from_ticks(0));

int main() {
    struct Case {
        const char* name;
        std::int64_t ticks;
        bool accepted;
    };

    constexpr std::array cases{
        Case{"minimum signed value", std::numeric_limits<std::int64_t>::min(), false},
        Case{"negative price", -1, false},
        Case{"zero price", 0, false},
        Case{"minimum valid price", 1, true},
        Case{"ordinary price", 10'001, true},
        Case{"maximum valid price", std::numeric_limits<std::int64_t>::max(), true},
    };

    for (const auto& test : cases) {
        const auto price = Price::from_ticks(test.ticks);
        if (price.has_value() != test.accepted) {
            std::cerr << test.name << ": unexpected acceptance result\n";
            return 1;
        }
        if (price && price->ticks() != test.ticks) {
            std::cerr << test.name << ": tick count changed\n";
            return 1;
        }
    }

    const auto low = Price::from_ticks(1).value();
    const auto middle = Price::from_ticks(10'001).value();
    const auto high = Price::from_ticks(std::numeric_limits<std::int64_t>::max()).value();
    if (!(low < middle && middle < high && high > low)) {
        std::cerr << "price ordering does not follow tick ordering\n";
        return 1;
    }
    if (middle != Price::from_ticks(10'001).value() || low == high) {
        std::cerr << "price equality does not follow tick equality\n";
        return 1;
    }
}
