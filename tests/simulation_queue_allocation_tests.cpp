#include <marketlab/simulation.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <utility>
#include <variant>

namespace {

std::ptrdiff_t allocations_before_failure = -1;

} // namespace

void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) {
        throw std::bad_alloc{};
    }
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) {
        return allocation;
    }
    throw std::bad_alloc{};
}

void operator delete(void* allocation) noexcept {
    std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
    ::operator delete(allocation);
}

void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}

void operator delete[](void* allocation) noexcept {
    ::operator delete(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    ::operator delete(allocation);
}

bool test_schedule_allocations() {
    using marketlab::CancelRequest;
    using marketlab::OrderRequest;
    using marketlab::SimulationEvent;
    using marketlab::SimulationQueue;
    std::size_t failures{};
    for (std::size_t existing = 0; existing <= 65; ++existing) {
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            SimulationQueue queue;
            std::array<SimulationEvent, 66> expected;
            for (std::size_t index = 0; index < existing; ++index) {
                const OrderRequest request{.timestamp = {(index * 37) % 17}, .order_id = {1000 - index}};
                const auto sequence = queue.schedule(request);
                if (!sequence) return false;
                expected[index] = {*sequence, request};
            }
            const auto* previous = queue.next();
            const CancelRequest request{.timestamp = {5}, .participant_id = {999}, .target_order_id = {123}};
            bool failed = false;
            allocations_before_failure = fault;
            try {
                const auto sequence = queue.schedule(request);
                allocations_before_failure = -1;
                if (!sequence || sequence->value != existing + 1) return false;
            } catch (const std::bad_alloc&) {
                allocations_before_failure = -1;
                failed = true;
                ++failures;
            }
            if (failed) {
                if (queue.size() != existing || queue.last_sequence().value != existing || queue.next() != previous) {
                    return false;
                }
                const auto retry = queue.schedule(request);
                if (!retry || retry->value != existing + 1) return false;
            }
            expected[existing] = {{existing + 1}, request};
            if (queue.size() != existing + 1 || queue.last_sequence().value != existing + 1) return false;
            std::sort(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(existing + 1), marketlab::scheduled_before);
            for (std::size_t index = 0; index <= existing; ++index) {
                // Removing an arrival must not allocate, even after an insertion retry.
                allocations_before_failure = 0;
                const auto event = queue.pop_next();
                allocations_before_failure = -1;
                if (!event || event->sequence != expected[index].sequence ||
                    marketlab::scheduled_time(*event) != marketlab::scheduled_time(expected[index]) ||
                    event->request.index() != expected[index].request.index()) {
                    return false;
                }
                if (const auto* order = std::get_if<OrderRequest>(&event->request)) {
                    const auto& original = std::get<OrderRequest>(expected[index].request);
                    if (order->order_id != original.order_id || order->quantity_units != original.quantity_units) return false;
                } else {
                    const auto& cancel = std::get<CancelRequest>(event->request);
                    if (cancel.participant_id != request.participant_id || cancel.target_order_id != request.target_order_id) return false;
                }
            }
            allocations_before_failure = 0;
            const auto absent = queue.pop_next();
            allocations_before_failure = -1;
            if (absent || !queue.empty() || queue.next() || queue.last_sequence().value != existing + 1) return false;
            if (!failed) break;
        }
    }
    return failures > 0;
}

bool test_delivery_allocations() {
    using namespace marketlab;
    enum class Scenario { Limit, Market, Cancel, RejectedOrder, RejectedCancel };
    for (const auto scenario : {Scenario::Limit, Scenario::Market, Scenario::Cancel,
        Scenario::RejectedOrder, Scenario::RejectedCancel}) {
        bool observed_failure = false;
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            const std::array participants{ParticipantId{1}, ParticipantId{2}};
            Simulation simulation{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
            const OrderRequest bid{{0}, {1}, {1}, {900}, Side::Buy, OrderType::Limit, 3, 100};
            const OrderRequest ask{{0}, {1}, {2}, {2}, Side::Sell, OrderType::Limit, 5, 101};
            if (!simulation.schedule(bid) || !simulation.schedule(ask)) return false;
            for (int index = 0; index < 2; ++index) {
                const auto prepared = simulation.step();
                if (!prepared || !*prepared) return false;
            }
            SimulationRequest request = OrderRequest{{10}, {1}, {1}, {50}, Side::Buy, OrderType::Limit, 7, 102};
            if (scenario == Scenario::Market) {
                request = OrderRequest{{10}, {1}, {1}, {50}, Side::Buy, OrderType::Market, 7, std::nullopt};
            } else if (scenario == Scenario::Cancel || scenario == Scenario::RejectedCancel) {
                request = CancelRequest{{10}, {1}, {scenario == Scenario::Cancel ? 2U : 1U}, {2}};
            } else if (scenario == Scenario::RejectedOrder) {
                std::get<OrderRequest>(request).participant_id = {3};
            }
            if (!simulation.schedule(CancelRequest{{20}, {1}, {1}, {777}}) ||
                !simulation.schedule(request) ||
                !simulation.schedule(OrderRequest{{30}, {1}, {2}, {77}, Side::Buy, OrderType::Limit, 1, 99})) return false;
            const auto& exchange = simulation.exchange();
            const auto& queue = simulation.queue();
            const auto* pending = queue.next();
            const auto* best_bid = exchange.book().best_bid();
            const auto* best_ask = exchange.book().best_ask();
            std::optional<SimulationStep> delivered;
            bool failed = false;
            allocations_before_failure = fault;
            try {
                auto result = simulation.step();
                allocations_before_failure = -1;
                if (!result || !*result) return false;
                delivered = std::move(result->value());
            } catch (const std::bad_alloc&) {
                allocations_before_failure = -1;
                failed = true;
                observed_failure = true;
            }
            if (failed) {
                if (queue.size() != 3 || queue.last_sequence().value != 5 || queue.next() != pending ||
                    pending->sequence.value != 4 || simulation.current_time().nanoseconds != 0 ||
                    exchange.last_request_sequence().value != 2 || exchange.last_event_sequence().value != 4 ||
                    exchange.cumulative_volume() != AggregateQuantity{} || exchange.last_trade() ||
                    exchange.book().order_count() != 2 || exchange.book().best_bid() != best_bid ||
                    exchange.book().best_ask() != best_ask || best_bid->remaining_quantity.units() != 3 ||
                    best_ask->remaining_quantity.units() != 5 || best_bid->order.arrival_sequence.value != 1 ||
                    best_ask->order.arrival_sequence.value != 2 || exchange.book().find({50}) ||
                    !exchange.book().check_invariants()) return false;
                auto retry = simulation.step();
                if (!retry || !*retry) return false;
                delivered = std::move(retry->value());
            }
            const bool matched = scenario == Scenario::Limit || scenario == Scenario::Market;
            if (!delivered || delivered->arrival.sequence.value != 4 || delivered->events.size() != (matched ? 5U : 1U) ||
                simulation.current_time().nanoseconds != 10 || queue.size() != 2 || queue.last_sequence().value != 5 ||
                !queue.next() || queue.next()->sequence.value != 3 || exchange.last_request_sequence().value != 3 ||
                exchange.last_event_sequence().value != (matched ? 9U : 5U) ||
                exchange.cumulative_volume() != AggregateQuantity{0, matched ? 5U : 0U} ||
                exchange.last_trade().has_value() != matched || !exchange.book().check_invariants()) return false;
            if (matched) {
                const auto* execution = std::get_if<Execution>(&delivered->events[1]);
                if (!execution || execution->quantity.units() != 5 || execution->price.ticks() != 101 ||
                    execution->aggressive_order_id.value != 50 || execution->resting_order_id.value != 2 ||
                    exchange.book().find({2})) return false;
                if (scenario == Scenario::Limit) {
                    const auto* remainder = exchange.book().find({50});
                    if (!remainder || remainder->remaining_quantity.units() != 2 ||
                        remainder->order.arrival_sequence.value != 3 || exchange.book().order_count() != 2) return false;
                } else if (exchange.book().find({50}) || exchange.book().order_count() != 1 ||
                    !std::holds_alternative<MarketRemainderExpired>(delivered->events.back())) return false;
            } else if (scenario == Scenario::Cancel) {
                const auto* cancelled = std::get_if<OrderCancelled>(&delivered->events.front());
                if (!cancelled || cancelled->cancelled_quantity.units() != 5 || exchange.book().find({2}) ||
                    exchange.book().order_count() != 1) return false;
            } else {
                if (!exchange.book().find({2}) || exchange.book().find({50}) || exchange.book().order_count() != 2) return false;
                if (scenario == Scenario::RejectedOrder) {
                    const auto* rejection = std::get_if<OrderRejected>(&delivered->events.front());
                    if (!rejection || rejection->reason != RejectionReason::InvalidParticipant) return false;
                } else {
                    const auto* rejection = std::get_if<CancelRejected>(&delivered->events.front());
                    if (!rejection || rejection->reason != RejectionReason::NotOwner) return false;
                }
            }
            // Previously scheduled later arrivals survive both failure and successful retry.
            for (const auto sequence : {3U, 5U}) {
                const auto later = simulation.step();
                if (!later || !*later || later->value().arrival.sequence.value != sequence) return false;
            }
            if (!queue.empty() || simulation.current_time().nanoseconds != 30 || !exchange.book().find({77})) return false;
            if (!failed) {
                if (!observed_failure) return false;
                break;
            }
        }
    }
    return true;
}

int main() {
    if (!test_delivery_allocations()) {
        std::cerr << "simulation delivery allocation failure lost an arrival or changed committed state\n";
        return 1;
    }
    if (!test_schedule_allocations()) {
        std::cerr << "simulation queue allocation failure changed state or prevented retry\n";
        return 1;
    }
}
