#pragma once

#include <marketlab/simulation_event.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace marketlab {

enum class IngressPushError { Closed, InvalidTimestamp, SequenceExhausted, Full };
enum class IngressPopError { Empty, Closed };

// One serialized producer and one serialized consumer. Requests are already in
// logical delivery order; this FIFO does not sort or validate exchange fields.
class BoundedIngressQueue {
public:
    explicit BoundedIngressQueue(std::size_t capacity) : slots_(capacity) {
        if (capacity == 0) throw std::invalid_argument{"ingress capacity must be positive"};
    }

    BoundedIngressQueue(const BoundedIngressQueue&) = delete;
    BoundedIngressQueue& operator=(const BoundedIngressQueue&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock{mutex_};
        return size_;
    }

    [[nodiscard]] bool is_closed() const {
        const std::lock_guard lock{mutex_};
        return closed_;
    }

    // Failed pushes change neither the accepted-time watermark nor the sequence.
    [[nodiscard]] std::expected<SimulationSequence, IngressPushError> try_push(SimulationRequest request) {
        const std::lock_guard lock{mutex_};
        if (closed_) return std::unexpected{IngressPushError::Closed};
        const auto time = std::visit([](const auto& value) { return value.timestamp; }, request);
        if (time < last_time_) return std::unexpected{IngressPushError::InvalidTimestamp};
        if (last_sequence_.value == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected{IngressPushError::SequenceExhausted};
        }
        if (size_ == slots_.size()) return std::unexpected{IngressPushError::Full};
        const SimulationSequence sequence{last_sequence_.value + 1};
        static_assert(std::is_nothrow_move_constructible_v<SimulationRequest>);
        static_assert(std::is_nothrow_move_assignable_v<SimulationEvent>);
        slots_[tail_] = SimulationEvent{sequence, std::move(request)};
        if (++tail_ == slots_.size()) tail_ = 0;
        ++size_;
        last_sequence_ = sequence;
        last_time_ = time;
        return sequence;
    }

    // A returned arrival is owned by the consumer, even after its slot is reused.
    [[nodiscard]] std::expected<SimulationEvent, IngressPopError> try_pop() {
        const std::lock_guard lock{mutex_};
        if (size_ == 0) {
            return std::unexpected{closed_ ? IngressPopError::Closed : IngressPopError::Empty};
        }
        static_assert(std::is_nothrow_copy_constructible_v<SimulationEvent>);
        static_assert(std::is_nothrow_constructible_v<std::expected<SimulationEvent, IngressPopError>, SimulationEvent&&>);
        auto event = slots_[head_];
        if (++head_ == slots_.size()) head_ = 0;
        --size_;
        return event;
    }

    // Producer-side close preserves every accepted arrival for draining.
    void close() {
        const std::lock_guard lock{mutex_};
        closed_ = true;
    }

private:
    std::vector<SimulationEvent> slots_;
    mutable std::mutex mutex_;
    std::size_t head_{};
    std::size_t tail_{};
    std::size_t size_{};
    LogicalTime last_time_{};
    SimulationSequence last_sequence_{};
    bool closed_{};
};

} // namespace marketlab
