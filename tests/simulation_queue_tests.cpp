#include <marketlab/simulation_queue.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

using marketlab::CancelRequest;
using marketlab::OrderRequest;
using marketlab::SimulationEvent;
using marketlab::SimulationQueue;
using marketlab::SimulationRequest;
using marketlab::SimulationSequence;
using marketlab::scheduled_time;

constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
static_assert(marketlab::detail::next_simulation_sequence({0}) == SimulationSequence{1});
static_assert(marketlab::detail::next_simulation_sequence({maximum - 1}) == SimulationSequence{maximum});
static_assert(!marketlab::detail::next_simulation_sequence({maximum}));
static_assert(!std::is_copy_constructible_v<SimulationQueue>);
static_assert(!std::is_move_constructible_v<SimulationQueue>);
static_assert(noexcept(std::declval<SimulationQueue&>().pop_next()));

bool same_request(const SimulationRequest& left, const SimulationRequest& right) {
    return std::visit([&](const auto& request) {
        const auto* other = std::get_if<std::decay_t<decltype(request)>>(&right);
        if (!other || request.timestamp != other->timestamp || request.instrument_id != other->instrument_id ||
            request.participant_id != other->participant_id) {
            return false;
        }
        if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
            return request.order_id == other->order_id && request.side == other->side && request.type == other->type &&
                request.quantity_units == other->quantity_units && request.limit_price_ticks == other->limit_price_ticks;
        } else {
            return request.target_order_id == other->target_order_id;
        }
    }, left);
}

bool test_arrival_ordering() {
    SimulationQueue queue;
    if (!queue.empty() || queue.size() != 0 || queue.next() || queue.pop_next() || queue.last_sequence().value != 0) {
        return false;
    }
    const std::array<SimulationRequest, 6> requests{
        OrderRequest{.timestamp = {10}, .order_id = {1}},
        CancelRequest{.timestamp = {5}, .target_order_id = {999}},
        OrderRequest{.timestamp = {5}, .order_id = {999}},
        OrderRequest{.timestamp = {5}, .order_id = {1}},
        CancelRequest{.timestamp = {0}, .target_order_id = {1}},
        OrderRequest{.timestamp = {maximum}, .order_id = {maximum}},
    };
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto assigned = queue.schedule(requests[index]);
        if (!assigned || assigned->value != index + 1 || queue.last_sequence() != *assigned ||
            queue.size() != index + 1 || queue.empty()) {
            return false;
        }
    }
    constexpr std::array<std::size_t, 6> sequence{5, 2, 3, 4, 1, 6};
    for (const auto expected : sequence) {
        const auto* next = queue.next();
        if (!next || next->sequence.value != expected || !same_request(next->request, requests[expected - 1])) {
            return false;
        }
        const auto event = queue.pop_next();
        if (!event || event->sequence.value != expected || !same_request(event->request, requests[expected - 1]) ||
            queue.last_sequence().value != requests.size()) {
            return false;
        }
    }
    return queue.empty() && queue.size() == 0 && !queue.next() && !queue.pop_next();
}

bool test_interleaved_insertion() {
    SimulationQueue queue;
    if (!queue.schedule(OrderRequest{.timestamp = {20}, .order_id = {999}}) ||
        !queue.schedule(OrderRequest{.timestamp = {10}, .order_id = {1}})) {
        return false;
    }
    const auto earlier = queue.pop_next();
    const auto assigned = queue.schedule(CancelRequest{.timestamp = {20}, .target_order_id = {999}});
    if (!earlier || earlier->sequence.value != 2 || !assigned || assigned->value != 3) {
        return false;
    }
    for (const auto sequence : {1U, 3U}) {
        const auto event = queue.pop_next();
        if (!event || event->sequence.value != sequence || scheduled_time(*event).nanoseconds != 20) {
            return false;
        }
    }
    const auto resumed = queue.schedule(OrderRequest{.timestamp = {30}});
    const auto event = queue.pop_next();
    return resumed && resumed->value == 4 && event && event->sequence == *resumed && queue.empty();
}

bool test_many_arrivals_and_payloads() {
    SimulationQueue queue;
    std::array<SimulationEvent, 256> expected;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto time = index == 0 ? maximum : (index * 37) % 17;
        const auto id = maximum - index;
        SimulationRequest request;
        if (index % 2 == 0) {
            const bool malformed = index % 4 == 0;
            const bool market = index % 8 == 2;
            request = OrderRequest{.timestamp = {time}, .instrument_id = {id},
                .participant_id = {malformed ? 0 : id}, .order_id = {id},
                .side = malformed ? static_cast<marketlab::Side>(0) :
                    (market ? marketlab::Side::Buy : marketlab::Side::Sell),
                .type = malformed ? static_cast<marketlab::OrderType>(0) :
                    (market ? marketlab::OrderType::Market : marketlab::OrderType::Limit),
                .quantity_units = malformed ? std::numeric_limits<std::int64_t>::min() :
                    std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(index),
                .limit_price_ticks = market ? std::nullopt :
                    std::optional<std::int64_t>{malformed ? 0 : static_cast<std::int64_t>(100 + index)}};
        } else {
            request = CancelRequest{.timestamp = {time}, .instrument_id = {0}, .participant_id = {id}, .target_order_id = {id}};
        }
        expected[index] = SimulationEvent{{index + 1}, request};
        const auto assigned = queue.schedule(request);
        if (!assigned || *assigned != expected[index].sequence || !same_request(request, expected[index].request)) {
            return false;
        }
    }
    std::sort(expected.begin(), expected.end(), marketlab::scheduled_before);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto event = queue.pop_next();
        if (!event || !marketlab::is_valid(*event) || event->sequence != expected[index].sequence ||
            !same_request(event->request, expected[index].request) || queue.size() != expected.size() - index - 1) {
            return false;
        }
    }
    return queue.empty() && queue.last_sequence().value == expected.size();
}

int main() {
    if (!test_arrival_ordering() || !test_interleaved_insertion() || !test_many_arrivals_and_payloads()) {
        std::cerr << "simulation queue ordering, sequence, or request payload differs\n";
        return 1;
    }
}
