#include "multi_instrument_test_support.hpp"

#include <marketlab/bounded_ingress_queue.hpp>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

using namespace multi_test;

static_assert(!std::is_copy_constructible_v<BoundedIngressQueue>);
static_assert(!std::is_move_constructible_v<BoundedIngressQueue>);
static_assert(noexcept(std::declval<const BoundedIngressQueue&>().capacity()));

bool same_request(const SimulationRequest& left, const SimulationRequest& right) {
    return std::visit([&](const auto& request) {
        const auto* other = std::get_if<std::decay_t<decltype(request)>>(&right);
        if (!other || request.timestamp != other->timestamp || request.instrument_id != other->instrument_id ||
            request.participant_id != other->participant_id) return false;
        if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
            return request.order_id == other->order_id && request.side == other->side && request.type == other->type &&
                request.quantity_units == other->quantity_units && request.limit_price_ticks == other->limit_price_ticks;
        } else return request.target_order_id == other->target_order_id;
    }, left);
}

void test_full_retry_and_drain() {
    BoundedIngressQueue queue{3};
    require(queue.capacity() == 3 && queue.size() == 0 && !queue.is_closed() &&
        queue.try_pop().error() == IngressPopError::Empty, "new ingress state differs");
    auto source = order(1, 9, 1, Side::Buy, 100, 3, 10);
    require(queue.try_push(source)->value == 1, "first ingress sequence differs");
    source.quantity_units = 99;
    const auto second = cancel(1, 9, 1, 10);
    const auto third = order(2, 1, 2, Side::Sell, 200, 4, 11);
    require(queue.try_push(second)->value == 2 && queue.try_push(third)->value == 3 && queue.size() == 3,
        "equal-time insertion or cross-instrument FIFO failed");
    require(queue.try_push(cancel(1, 9, 1, 100)).error() == IngressPushError::Full &&
        queue.try_push(cancel(1, 9, 1, 9)).error() == IngressPushError::InvalidTimestamp && queue.size() == 3,
        "full/invalid push changed pending arrivals or diagnostic precedence");
    const auto first = queue.try_pop();
    require(first && first->sequence.value == 1 && std::get<OrderRequest>(first->request).quantity_units == 3,
        "ingress borrowed caller storage");
    const auto fourth = cancel(2, 1, 2, 12);
    require(queue.try_push(fourth)->value == 4, "full refusal advanced sequence or accepted-time watermark");
    queue.close();
    queue.close();
    require(queue.is_closed() && queue.size() == 3 &&
        queue.try_push(cancel(1, 9, 1, 0)).error() == IngressPushError::Closed,
        "close dropped accepted arrivals or admitted a new push");
    const std::array<SimulationRequest, 3> expected{second, third, fourth};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto event = queue.try_pop();
        require(event && event->sequence.value == index + 2 && same_request(event->request, expected[index]),
            "closed drain changed payloads, sequences or FIFO order");
    }
    require(queue.size() == 0 && queue.try_pop().error() == IngressPopError::Closed &&
        std::get<OrderRequest>(first->request).quantity_units == 3, "drain or slot reuse invalidated an owned result");
}

void test_time_and_payload_boundaries() {
    for (const auto capacity : {1U, 7U}) {
        BoundedIngressQueue queue{capacity};
        auto request = order(0, 0, 0, static_cast<Side>(0), std::numeric_limits<std::int64_t>::min(),
            std::numeric_limits<std::int64_t>::min(), 0);
        request.type = static_cast<OrderType>(0);
        require(queue.try_push(request)->value == 1 && same_request(queue.try_pop()->request, request),
            "ingress applied exchange field validation or changed malformed payload");
        require(queue.try_push(cancel(0, 0, 0, 20))->value == 2 && queue.try_pop().has_value(), "drained queue cannot resume");
        require(queue.try_push(cancel(0, 0, 0, 19)).error() == IngressPushError::InvalidTimestamp &&
            queue.try_push(cancel(0, 0, 0, 20))->value == 3 && queue.try_pop().has_value(),
            "drain reset time/sequence or equal-time retry failed");
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        const SimulationRequest boundary = CancelRequest{{maximum}, {maximum}, {maximum}, {maximum}};
        require(queue.try_push(boundary)->value == 4 && same_request(queue.try_pop()->request, boundary) &&
            queue.try_push(boundary)->value == 5 && same_request(queue.try_pop()->request, boundary),
            "maximum logical time or IDs did not survive FIFO delivery");
        require(queue.try_push(request).error() == IngressPushError::InvalidTimestamp,
            "time watermark reset after draining maximum timestamp");
        queue.close();
        require(queue.try_pop().error() == IngressPopError::Closed, "empty close did not end delivery");
    }
    for (const auto capacity : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
        bool refused = false;
        try { BoundedIngressQueue invalid{capacity}; }
        catch (const std::invalid_argument&) { refused = capacity == 0; }
        catch (const std::length_error&) { refused = capacity != 0; }
        require(refused, "invalid or unrepresentable capacity accepted");
    }
    const auto owned = [] {
        BoundedIngressQueue queue{1};
        require(queue.try_push(order(1, 1, 1, Side::Buy, 100, 2, 0)).has_value(), "owned result setup failed");
        return queue.try_pop();
    }();
    require(owned && std::get<OrderRequest>(owned->request).quantity_units == 2,
        "popped request did not survive queue destruction");
}

SimulationRequest input(std::uint64_t index) {
    if (index % 2 == 0) {
        auto request = order(index % 5, 100000 - index, index % 3,
            index % 4 == 0 ? Side::Buy : Side::Sell, static_cast<std::int64_t>(index),
            -static_cast<std::int64_t>(index), index / 4);
        if (index % 8 == 0) { request.type = OrderType::Market; request.limit_price_ticks.reset(); }
        return request;
    }
    return cancel(index % 5, 100000 - index, index % 3, index / 4);
}

void test_concurrent_delivery(std::size_t capacity) {
    BoundedIngressQueue queue{capacity};
    constexpr std::uint64_t count = 20000;
    std::atomic<bool> valid{true};
    std::uint64_t received = 0;
    std::jthread consumer{[&] {
        while (true) {
            const auto event = queue.try_pop();
            if (!event) {
                if (event.error() == IngressPopError::Closed) return;
                std::this_thread::yield();
                continue;
            }
            if (received >= count || event->sequence.value != received + 1 ||
                !same_request(event->request, input(received))) {
                valid.store(false);
                return;
            }
            ++received;
        }
    }};
    for (std::uint64_t index = 0; index < count && valid.load(); ++index) {
        while (true) {
            const auto pushed = queue.try_push(input(index));
            if (pushed) {
                if (pushed->value != index + 1) valid.store(false);
                break;
            }
            if (pushed.error() != IngressPushError::Full || !valid.load()) {
                valid.store(false);
                break;
            }
            std::this_thread::yield();
        }
    }
    queue.close();
    consumer.join();
    require(valid.load() && received == count && queue.size() == 0 && queue.is_closed(),
        "concurrent bounded delivery lost, duplicated or reordered an accepted arrival");
}

void test_concurrent_empty_close() {
    BoundedIngressQueue queue{1};
    std::atomic<bool> observed_empty{false};
    bool observed_close = false;
    std::jthread consumer{[&] {
        while (true) {
            const auto event = queue.try_pop();
            if (event) return;
            if (event.error() == IngressPopError::Closed) { observed_close = true; return; }
            observed_empty.store(true);
            std::this_thread::yield();
        }
    }};
    while (!observed_empty.load()) std::this_thread::yield();
    queue.close();
    consumer.join();
    require(observed_close, "consumer did not observe close after an empty poll");
}

int main() {
    try {
        test_full_retry_and_drain();
        test_time_and_payload_boundaries();
        for (const auto capacity : {1U, 2U, 7U, 64U}) test_concurrent_delivery(capacity);
        test_concurrent_empty_close();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
