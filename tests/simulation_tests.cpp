#include <marketlab/simulation_event.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <variant>

using marketlab::CancelRequest;
using marketlab::EventSequence;
using marketlab::LogicalTime;
using marketlab::OrderId;
using marketlab::OrderRequest;
using marketlab::RequestSequence;
using marketlab::SimulationEvent;
using marketlab::SimulationSequence;
using marketlab::is_valid;
using marketlab::scheduled_before;
using marketlab::scheduled_time;

static_assert(!std::is_convertible_v<SimulationSequence, RequestSequence>);
static_assert(!std::is_convertible_v<SimulationSequence, EventSequence>);
static_assert(!std::is_convertible_v<OrderId, SimulationSequence>);
static_assert(!std::is_convertible_v<std::uint64_t, SimulationSequence>);
static_assert(!is_valid(SimulationEvent{}));
constexpr SimulationEvent first{{1}, OrderRequest{.timestamp = {0}}};
constexpr SimulationEvent second{{2}, CancelRequest{.timestamp = {0}}};
static_assert(is_valid(first));
static_assert(scheduled_time(first) == LogicalTime{0});
static_assert(scheduled_before(first, second));
static_assert(!scheduled_before(second, first));
static_assert(!scheduled_before(first, first));

int main() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto value : std::array<std::uint64_t, 3>{0, 1, maximum}) {
        const SimulationEvent event{{value}, OrderRequest{}};
        if (is_valid(SimulationSequence{value}) != (value != 0) ||
            is_valid(event) != (value != 0)) {
            std::cerr << "unexpected simulation-sequence validity\n";
            return 1;
        }
    }

    // Equal-time priority comes from scheduling insertion, never order ID or kind.
    std::array events{
        SimulationEvent{{1}, OrderRequest{.timestamp = {10}, .order_id = {1}}},
        SimulationEvent{{2}, CancelRequest{.timestamp = {5}, .target_order_id = {999}}},
        SimulationEvent{{3}, OrderRequest{.timestamp = {5}, .order_id = {999}}},
        SimulationEvent{{4}, OrderRequest{.timestamp = {5}, .order_id = {1}}},
        SimulationEvent{{5}, CancelRequest{.timestamp = {0}, .target_order_id = {1}}},
        SimulationEvent{{maximum}, OrderRequest{.timestamp = {maximum}, .order_id = {1}}},
    };
    std::sort(events.begin(), events.end(), scheduled_before);
    constexpr std::array<std::uint64_t, 6> expected{5, 2, 3, 4, 1, maximum};
    constexpr std::array<std::uint64_t, 6> expected_times{0, 5, 5, 5, 10, maximum};
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (!is_valid(events[index]) || events[index].sequence.value != expected[index] ||
            scheduled_time(events[index]).nanoseconds != expected_times[index]) {
            std::cerr << "scheduled requests are not ordered by time then insertion\n";
            return 1;
        }
        for (std::size_t other = 0; other < events.size(); ++other) {
            if (scheduled_before(events[index], events[other]) != (index < other)) {
                std::cerr << "scheduled ordering is not strict\n";
                return 1;
            }
        }
    }
    const auto& stored_cancel = std::get<CancelRequest>(events[1].request);
    const auto& stored_order = std::get<OrderRequest>(events[2].request);
    if (stored_cancel.target_order_id != OrderId{999} || stored_order.order_id != OrderId{999}) {
        std::cerr << "scheduled sorting changed request payloads\n";
        return 1;
    }

    auto malformed = OrderRequest{};
    malformed.timestamp = {maximum};
    malformed.quantity_units = std::numeric_limits<std::int64_t>::min();
    malformed.limit_price_ticks = 0;
    const SimulationEvent raw{{1}, malformed};
    const auto& preserved = std::get<OrderRequest>(raw.request);
    if (!is_valid(raw) || scheduled_time(raw) != LogicalTime{maximum} ||
        preserved.quantity_units != malformed.quantity_units || preserved.limit_price_ticks != 0 ||
        preserved.order_id != OrderId{} || !is_valid(SimulationEvent{{1}, CancelRequest{}})) {
        std::cerr << "scheduling did not preserve a malformed submission\n";
        return 1;
    }

    // Duplicate keys compare equivalent; uniqueness requires future scheduler state.
    const SimulationEvent same_key{{1}, CancelRequest{.timestamp = {0}}};
    if (scheduled_before(first, same_key) || scheduled_before(same_key, first)) {
        std::cerr << "payload kind affected identical scheduling keys\n";
        return 1;
    }
}
