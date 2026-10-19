#include "partitioned_test_support.hpp"

#include <marketlab/bounded_ingress_queue.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
std::ptrdiff_t budget = -1;
}

#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void* operator new(std::size_t bytes) {
    if (budget >= 0 && budget-- == 0) throw std::bad_alloc{};
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

void prepare(Exchange& exchange) {
    require(exchange.process_order(order(1, 1, 1, Side::Sell, 100, 5, 0)).has_value() &&
        exchange.process_order(order(1, 2, 1, Side::Sell, 101, 7, 1)).has_value(), "retry fixture setup failed");
}

void test_owned_arrival_retry() {
    auto market = order(1, 3, 2, Side::Buy, 0, 10, 2);
    market.type = OrderType::Market;
    market.limit_price_ticks.reset();
    const std::array<SimulationRequest, 6> requests{
        order(1, 3, 2, Side::Buy, 99, 4, 2), order(1, 3, 2, Side::Buy, 101, 10, 2), market,
        cancel(1, 1, 1, 2), order(1, 3, 2, Side::Buy, 99, 0, 2), cancel(1, 1, 2, 2)};
    const auto config = configurations()[1];
    const SimulationRequest following = cancel(1, 2, 1, 3);
    for (const auto& request : requests) {
        Exchange reference{config, participants};
        prepare(reference);
        const auto first_expected = isolated_process(reference, request);
        const auto next_expected = isolated_process(reference, following);
        require(first_expected && next_expected, "retry reference failed");
        std::size_t failures = 0;
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            Exchange exchange{config, participants};
            prepare(exchange);
            BoundedIngressQueue ingress{1};
            require(ingress.try_push(request)->value == 1, "first transport insertion failed");
            const auto in_flight = ingress.try_pop();
            require(in_flight && in_flight->sequence.value == 1 && ingress.try_push(following)->value == 2 &&
                ingress.try_push(following).error() == IngressPushError::Full, "in-flight arrival did not free exactly one slot");
            ingress.close();
            const auto before = state(exchange);
            std::optional<std::vector<ExchangeEvent>> events;
            bool failed = false;
            budget = fault;
            try {
                auto processed = isolated_process(exchange, in_flight->request);
                budget = -1;
                require(processed.has_value(), "allocation became an engine diagnostic");
                events.emplace(std::move(*processed));
            } catch (const std::bad_alloc&) {
                budget = -1;
                failed = true;
                ++failures;
            }
            require(ingress.is_closed() && ingress.size() == 1 && in_flight->sequence.value == 1 &&
                scheduled_time(*in_flight) == LogicalTime{2}, "exchange processing changed the retained arrival or queued successor");
            if (failed) {
                require(!events && state(exchange) == before, "failed exchange processing committed partial state");
                auto retried = isolated_process(exchange, in_flight->request);
                require(retried.has_value(), "owned in-flight request could not be retried");
                events.emplace(std::move(*retried));
            }
            require(events && event_text(*events) == event_text(*first_expected), "retry changed first-request events");
            const auto next = ingress.try_pop();
            require(next && next->sequence.value == 2 && scheduled_time(*next) == LogicalTime{3},
                "closed FIFO lost or overtook its retained in-flight predecessor");
            const auto actual_next = isolated_process(exchange, next->request);
            require(actual_next && event_text(*actual_next) == event_text(*next_expected) &&
                state(exchange) == state(reference) && exchange.book().check_invariants() &&
                ingress.try_pop().error() == IngressPopError::Closed, "retry-plus-drain changed final events or state");
            if (!failed) break;
        }
        require(failures != 0, "exchange allocation failures were not exercised");
    }
}

int main() {
    try {
        test_owned_arrival_retry();
    } catch (const std::exception& error) {
        budget = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
