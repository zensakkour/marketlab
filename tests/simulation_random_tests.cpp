#include <marketlab/simulation_random.hpp>

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

using marketlab::SimulationRandom;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

void test_full_range_and_no_draws() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto seed : std::array<std::uint64_t, 3>{0, 42, maximum}) {
        SimulationRandom random{seed};
        std::mt19937_64 reference{seed};
        for (int index = 0; index < 10000; ++index) {
            require(!random.uniform(2, 1) && random.uniform(maximum, maximum) == maximum &&
                random.uniform(0, 0) == 0, "invalid/singleton range differs");
            require(random.uniform(0, maximum) == reference(), "full range or no-draw state differs");
        }
    }
}

void test_mapping_and_rejection() {
    SimulationRandom random{42};
    std::mt19937_64 reference{42};
    for (int index = 0; index < 10000; ++index) {
        require(random.uniform(3, 10) == 3 + (reference() & 7), "power-of-two interval mapping differs");
    }
    SimulationRandom rejected{42};
    // The fourth engine word is below the acceptance threshold and is skipped.
    constexpr std::array<std::uint64_t, 4> expected{
        4706788815403344597ULL, 2564676540648719015ULL, 4651257987612965641ULL, 7438999416573663572ULL};
    for (const auto value : expected) {
        require(rejected.uniform(0, (std::uint64_t{1} << 63)) == value, "rejection-sampling golden sequence differs");
    }
}

void test_ranges_and_replay() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::array ranges{
        std::array<std::uint64_t, 2>{0, 9},
        std::array<std::uint64_t, 2>{maximum - 9, maximum},
        std::array<std::uint64_t, 2>{1, maximum},
        std::array<std::uint64_t, 2>{0, std::uint64_t{1} << 63}};
    SimulationRandom first{12345};
    SimulationRandom second{12345};
    std::array<std::size_t, 10> counts{};
    for (int index = 0; index < 100000; ++index) {
        for (const auto& range : ranges) {
            const auto value = first.uniform(range[0], range[1]);
            require(value && value == second.uniform(range[0], range[1]) &&
                *value >= range[0] && *value <= range[1], "bounded replay differs");
            if (range[1] == 9) ++counts[*value];
        }
    }
    for (const auto count : counts) require(count > 8000 && count < 12000, "small-range occupancy differs");
    auto checkpoint = first;
    require(first.uniform(0, maximum) == checkpoint.uniform(0, maximum), "random checkpoint differs");
}

int main() {
    static_assert(std::is_nothrow_copy_assignable_v<SimulationRandom>);
    try {
        test_full_range_and_no_draws();
        test_mapping_and_rejection();
        test_ranges_and_replay();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
