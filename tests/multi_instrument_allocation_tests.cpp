#include "multi_instrument_test_support.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <variant>

namespace {
std::ptrdiff_t allocations_before_failure = -1;
}

// Keep both replacement boundaries intact for GCC allocation-pair diagnostics.
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) throw std::bad_alloc{};
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

using namespace multi_test;

void prepare(MultiInstrumentExchange& exchange) {
    require(exchange.process_order(order(1, 1, 1, Side::Sell, 100, 3, 1)).has_value() &&
        exchange.process_order(order(1, 2, 1, Side::Sell, 101, 5, 1)).has_value() &&
        exchange.process_order(order(2, 1, 2, Side::Buy, 200, 9, 7)).has_value(),
        "allocation fixture preparation failed");
}

MultiInstrumentExchange::Result process(MultiInstrumentExchange& exchange, int scenario) {
    if (scenario == 3) return exchange.process_cancel(cancel(1, 1, 1, 2));
    if (scenario == 5) return exchange.process_cancel(cancel(1, 1, 2, 2));
    auto request = order(1, 3, 2, Side::Buy, scenario == 0 ? 99 : 101, 10, 2);
    if (scenario == 2) {
        request.type = OrderType::Market;
        request.limit_price_ticks.reset();
    }
    if (scenario == 4) request.quantity_units = 0;
    return exchange.process_order(request);
}

void test_constructor_failure() {
    auto configs = configurations();
    configs[0].symbol.assign(200, 'B');
    configs[1].symbol.assign(200, 'A');
    InstrumentRegistry registry{configs};
    const auto* borrowed = registry.find({1});
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        bool failed = false;
        allocations_before_failure = fault;
        try {
            MultiInstrumentExchange exchange{registry, participants};
            allocations_before_failure = -1;
            require(exchange.size() == 2 && exchange.find({1})->instrument().symbol == configs[1].symbol &&
                exchange.find({2})->instrument().symbol == configs[0].symbol, "constructed owner changed metadata");
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            failed = true;
            ++failures;
        }
        require(registry.find({1}) == borrowed && borrowed->symbol == configs[1].symbol,
            "failed owner construction changed source registry");
        MultiInstrumentExchange retry{registry, participants};
        require(retry.size() == 2 && retry.find({1})->book().empty() && retry.find({2})->book().empty() &&
            retry.find({1})->last_event_sequence().value == 0, "constructor retry retained partial state");
        if (!failed) break;
    }
    require(failures != 0, "owner constructor allocation failures not exercised");
}

void test_processing_failure_and_retry() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    for (int scenario = 0; scenario < 6; ++scenario) {
        MultiInstrumentExchange baseline{registry, participants};
        prepare(baseline);
        const auto expected = process(baseline, scenario);
        require(expected.has_value(), "baseline route failed");
        const auto expected_events = event_text(*expected);
        const auto expected_state = state(*baseline.find({1}));
        std::size_t failures = 0;
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            MultiInstrumentExchange exchange{registry, participants};
            prepare(exchange);
            const auto* a = exchange.find({1});
            const auto* b = exchange.find({2});
            const auto* metadata = &a->instrument();
            const auto before_a = state(*a), before_b = state(*b);
            bool failed = false;
            allocations_before_failure = fault;
            try {
                const auto result = process(exchange, scenario);
                allocations_before_failure = -1;
                require(result && event_text(*result) == expected_events, "successful routed result differs");
            } catch (const std::bad_alloc&) {
                allocations_before_failure = -1;
                failed = true;
                ++failures;
            }
            if (failed) {
                require(state(*a) == before_a && state(*b) == before_b && exchange.find({1}) == a &&
                    &a->instrument() == metadata, "routing allocation failure changed either instrument or a view");
                const auto retry = process(exchange, scenario);
                require(retry && event_text(*retry) == expected_events, "routing identical retry differs");
            }
            require(state(*a) == expected_state && state(*b) == before_b &&
                a->book().check_invariants() && b->book().check_invariants(),
                "routed commit/retry changed wrong book, consumed counters twice or lost liquidity");
            if (!failed) break;
        }
        require(failures != 0, "routed processing allocations not exercised");
    }
}

void test_nonallocating_paths() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs}, empty_registry;
    MultiInstrumentExchange exchange{registry, participants};
    prepare(exchange);
    const auto* a = exchange.find({1});
    const auto before_a = state(*a), before_b = state(*exchange.find({2}));
    const auto unknown_order = order(99, 1, 0, Side::Buy, -1, -1, UINT64_MAX);
    const auto unknown_cancel = cancel(0, 1, 0, UINT64_MAX);
    allocations_before_failure = 0;
    MultiInstrumentExchange empty{empty_registry, participants};
    const auto* found = exchange.find({1});
    const auto* missing = exchange.find({99});
    const auto* metadata = &found->instrument();
    const auto size = exchange.size();
    const auto is_empty = empty.empty();
    const auto new_order = exchange.process_order(unknown_order);
    const auto cancellation = exchange.process_cancel(unknown_cancel);
    allocations_before_failure = -1;
    require(found == a && !missing && metadata == &a->instrument() && size == 2 && is_empty &&
        !new_order && new_order.error() == MultiInstrumentError::UnknownInstrument &&
        !cancellation && cancellation.error() == MultiInstrumentError::UnknownInstrument &&
        state(*a) == before_a && state(*exchange.find({2})) == before_b,
        "read/unknown routing allocated or changed instrument state");
}

int main() {
    try {
        test_constructor_failure();
        test_processing_failure_and_retry();
        test_nonallocating_paths();
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
