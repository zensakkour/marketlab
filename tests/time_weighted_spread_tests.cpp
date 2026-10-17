#include <marketlab/time_weighted_spread.hpp>

#include "quantity_oracle.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
bool deny_allocations = false;
}

void* operator new(std::size_t bytes) {
    if (deny_allocations) throw std::bad_alloc{};
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

using namespace marketlab;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

constexpr bool exact_timeline() {
    TimeWeightedSpread observer;
    if (!observer.observe({5}, 2) || !observer.observe({10}, 4) ||
        !observer.observe({10}, std::nullopt) || !observer.observe({10}, 6)) return false;
    const auto result = observer.snapshot({20});
    return result && *result == SpreadStatistics{{0, 70}, 15, 5} &&
        result->mean() == MeanSpread{{0, 70}, 15};
}
static_assert(exact_timeline());
static_assert(noexcept(TimeWeightedSpread{}.observe({}, std::nullopt)));
static_assert(noexcept(TimeWeightedSpread{}.snapshot({})));

void test_intervals_and_refusal() {
    TimeWeightedSpread observer;
    require(observer.snapshot({10}) == SpreadStatistics{{}, 0, 10} &&
        !observer.snapshot({10})->mean(), "missing-only mean should be absent");
    require(observer.observe({0}, 2).has_value() && observer.observe({10}, 4).has_value(),
        "valid observation refused");
    const auto before = observer.snapshot({20});
    const auto past = observer.observe({9}, 8);
    const auto negative = observer.observe({20}, -1);
    const auto precedence = observer.observe({9}, -1);
    const auto past_read = observer.snapshot({9});
    require(!past && past.error() == SpreadObservationError::InvalidTimestamp &&
        !negative && negative.error() == SpreadObservationError::InvalidSpread &&
        !precedence && precedence.error() == SpreadObservationError::InvalidTimestamp &&
        !past_read && past_read.error() == SpreadObservationError::InvalidTimestamp &&
        observer.snapshot({20}) == before, "invalid observation/read changed state or diagnostic");
    require(observer.snapshot({10}) == SpreadStatistics{{0, 20}, 10, 0},
        "future projection advanced the observation cursor");
    const auto owned = *observer.snapshot({20});
    require(observer.observe({20}, std::nullopt).has_value() && observer.observe({30}, 0).has_value() &&
        observer.snapshot({40}) == SpreadStatistics{{0, 60}, 30, 10} &&
        owned == SpreadStatistics{{0, 60}, 20, 0}, "missing, zero or owned state differs");
}

void test_maximum_integral_and_no_allocation() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr auto spread = std::numeric_limits<std::int64_t>::max();
    TimeWeightedSpread observer;
    deny_allocations = true;
    const auto first = observer.observe({0}, spread);
    const auto split = observer.observe({maximum / 2}, spread);
    const auto projected = observer.snapshot({maximum});
    const auto committed = observer.observe({maximum}, std::nullopt);
    const auto result = observer.snapshot({maximum});
    const auto past = observer.snapshot({0});
    const auto invalid = observer.observe({maximum}, -1);
    const auto mean = result->mean();
    deny_allocations = false;
    require(first && split && committed && projected && result && *result == *projected && !past && !invalid &&
        result->tick_nanoseconds == SpreadIntegral{(maximum >> 1) - 1, (maximum >> 1) + 2} &&
        result->covered_nanoseconds == maximum && result->missing_nanoseconds == 0 &&
        mean == MeanSpread{result->tick_nanoseconds, maximum},
        "maximum-duration integral, carry or no-allocation path differs");
    require(to_string(result->tick_nanoseconds) == decimal_product(static_cast<std::uint64_t>(spread), maximum),
        "maximum integral differs from independent decimal product");
}

void test_generated_decimal_reconciliation() {
    std::mt19937_64 random{42};
    TimeWeightedSpread observer;
    std::uint64_t time = 0, covered = 0, missing = 0;
    std::optional<std::int64_t> previous;
    std::string integral = "0";
    for (int index = 0; index < 10000; ++index) {
        const auto duration = random() % 1000000000;
        time += duration;
        if (previous) {
            covered += duration;
            integral = add_decimal(integral, decimal_product(static_cast<std::uint64_t>(*previous), duration));
        } else missing += duration;
        const auto next = index % 4 == 0 ? std::nullopt :
            std::optional{static_cast<std::int64_t>(random() >> 1)};
        require(observer.observe({time}, next).has_value(), "generated observation refused");
        const auto result = observer.snapshot({time});
        require(result && to_string(result->tick_nanoseconds) == integral &&
            result->covered_nanoseconds == covered && result->missing_nanoseconds == missing &&
            covered + missing == time, "generated spread integral differs from decimal oracle");
        const auto future = observer.snapshot({time + 7});
        const auto future_integral = next ?
            add_decimal(integral, decimal_product(static_cast<std::uint64_t>(*next), 7)) : integral;
        require(future && to_string(future->tick_nanoseconds) == future_integral &&
            observer.snapshot({time}) == result, "projection changed observer or future integral");
        previous = next;
    }
}

int main() {
    try {
        test_intervals_and_refusal();
        test_maximum_integral_and_no_allocation();
        test_generated_decimal_reconciliation();
    } catch (const std::exception& error) {
        deny_allocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
