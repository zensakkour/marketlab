#include <marketlab/instrument_registry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
std::ptrdiff_t allocations_before_failure = -1;
}

void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) throw std::bad_alloc{};
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

using namespace marketlab;

static_assert(!std::is_copy_constructible_v<InstrumentRegistry>);
static_assert(!std::is_move_constructible_v<InstrumentRegistry>);
static_assert(std::is_same_v<decltype(std::declval<const InstrumentRegistry&>().find({1})), const Instrument*>);
static_assert(std::is_same_v<decltype(std::declval<const InstrumentRegistry&>().instruments()), std::span<const Instrument>>);
static_assert(noexcept(std::declval<const InstrumentRegistry&>().find({1})));
static_assert(noexcept(std::declval<const InstrumentRegistry&>().instruments()));

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

bool same_instrument(const Instrument& left, const Instrument& right) {
    return left.id == right.id && left.symbol == right.symbol &&
        left.tick_size.coefficient() == right.tick_size.coefficient() &&
        left.tick_size.scale() == right.tick_size.scale();
}

void check_registry(const InstrumentRegistry& registry, std::span<const Instrument> source) {
    require(registry.size() == source.size() && registry.empty() == source.empty(), "registry size differs");
    for (const auto& instrument : source) {
        const auto* found = registry.find(instrument.id);
        require(found && same_instrument(*found, instrument), "registry changed instrument configuration");
    }
    const auto view = registry.instruments();
    for (std::size_t index = 1; index < view.size(); ++index) {
        require(view[index - 1].id < view[index].id, "registry iteration is not increasing by ID");
    }
}

void test_owned_configuration_and_boundaries() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto cents = TickSize::from_decimal(1, 2).value();
    const auto precise = TickSize::from_decimal(std::numeric_limits<std::int64_t>::max(), 18).value();
    const auto whole = TickSize::from_decimal(5, 0).value();
    std::vector<Instrument> source{
        {{maximum}, "SAME", precise}, {{1}, "SAME", cents}, {{42}, "OTHER", whole}};
    InstrumentRegistry registry{source};
    check_registry(registry, source);
    const auto* borrowed = registry.find({maximum});
    const auto view = registry.instruments();
    source[0].symbol = "CHANGED";
    source[1].id = {99};
    source[2].tick_size = cents;
    source.clear();
    require(borrowed && borrowed->symbol == "SAME" && borrowed->tick_size.coefficient() == precise.coefficient() &&
        registry.find({1})->symbol == "SAME" && registry.find({42})->tick_size.coefficient() == 5 &&
        registry.find({maximum}) == borrowed && !registry.find({0}) && !registry.find({99}) &&
        view.data() == registry.instruments().data(), "registry borrowed caller configuration or changed a view");
    InstrumentRegistry independent{view};
    check_registry(independent, view);
    require(independent.find({maximum}) != borrowed, "separate registry shared instrument storage");

    const std::array original{
        Instrument{{3}, "THREE", cents}, Instrument{{1}, "ONE", cents}, Instrument{{2}, "TWO", whole}};
    auto permutation = original;
    std::ranges::sort(permutation, {}, &Instrument::id);
    do {
        InstrumentRegistry reordered{permutation};
        for (std::size_t index = 0; index < original.size(); ++index) {
            require(reordered.instruments()[index].id.value == index + 1, "input order changed registry iteration");
        }
        check_registry(reordered, original);
    } while (std::next_permutation(permutation.begin(), permutation.end(),
        [](const Instrument& left, const Instrument& right) { return left.id < right.id; }));
}

void test_invalid_configuration() {
    const auto tick = TickSize::from_decimal(1, 2).value();
    for (int test = 0; test < 4; ++test) {
        std::array source{Instrument{{1}, "FIRST", tick}, Instrument{{2}, "SECOND", tick}};
        if (test == 0) source[1].id = {0};
        if (test == 1) source[1].symbol.clear();
        if (test == 2) source[1] = source[0];
        if (test == 3) source[1].id = source[0].id;
        const auto before = source;
        bool refused = false;
        try { InstrumentRegistry invalid{source}; }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused && same_instrument(source[0], before[0]) && same_instrument(source[1], before[1]),
            "invalid/duplicate configuration accepted or source changed");
    }
}

void test_allocation_failure_and_reads() {
    const auto tick = TickSize::from_decimal(1, 2).value();
    const std::array source{
        Instrument{{3}, std::string(200, 'c'), tick},
        Instrument{{1}, std::string(200, 'a'), tick},
        Instrument{{2}, std::string(200, 'b'), tick}};
    const auto before = source;
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        bool failed = false;
        allocations_before_failure = fault;
        try {
            InstrumentRegistry registry{source};
            allocations_before_failure = -1;
            check_registry(registry, source);
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            failed = true;
            ++failures;
        }
        for (std::size_t index = 0; index < source.size(); ++index) {
            require(same_instrument(source[index], before[index]), "failed construction changed caller configuration");
        }
        InstrumentRegistry retry{source};
        check_registry(retry, source);
        if (!failed) break;
    }
    require(failures >= 4, "vector and owned-symbol allocations were not exercised");

    InstrumentRegistry registry{source};
    const auto* borrowed = registry.find({1});
    allocations_before_failure = 0;
    InstrumentRegistry empty;
    const auto view = registry.instruments();
    const auto* found = registry.find({1});
    const auto* missing = registry.find({99});
    const auto* zero = registry.find({0});
    const auto* no_instrument = empty.find({1});
    const auto size = registry.size();
    const auto empty_size = empty.size();
    const bool is_empty = empty.empty();
    allocations_before_failure = -1;
    require(found == borrowed && found == &view.front() && !missing && !zero && !no_instrument &&
        size == 3 && empty_size == 0 && is_empty, "empty construction/read allocated or returned wrong values");
}

int main() {
    try {
        test_owned_configuration_and_boundaries();
        test_invalid_configuration();
        test_allocation_failure_and_reads();
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
