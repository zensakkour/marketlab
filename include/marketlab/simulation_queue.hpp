#pragma once

#include <marketlab/simulation_event.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace marketlab {

namespace detail {

[[nodiscard]] constexpr std::optional<SimulationSequence> next_simulation_sequence(
    SimulationSequence last) noexcept {
    if (last.value == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return SimulationSequence{last.value + 1};
}

} // namespace detail

enum class SimulationQueueError {
    SequenceExhausted,
    CapacityExceeded,
};

// Owns pending arrivals; advancing logical time and delivering them belong to the runner.
class SimulationQueue {
public:
    SimulationQueue() = default;
    SimulationQueue(const SimulationQueue&) = delete;
    SimulationQueue& operator=(const SimulationQueue&) = delete;
    SimulationQueue(SimulationQueue&&) = delete;
    SimulationQueue& operator=(SimulationQueue&&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return events_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return events_.size();
    }

    [[nodiscard]] SimulationSequence last_sequence() const noexcept {
        return last_sequence_;
    }

    // Borrowed until the next successful schedule/pop operation.
    [[nodiscard]] const SimulationEvent* next() const noexcept {
        return events_.empty() ? nullptr : &events_.front();
    }

    // Commit sequence only after storage succeeds; malformed fields are preserved.
    [[nodiscard]] std::expected<SimulationSequence, SimulationQueueError> schedule(SimulationRequest request) {
        const auto sequence = detail::next_simulation_sequence(last_sequence_);
        if (!sequence) {
            return std::unexpected{SimulationQueueError::SequenceExhausted};
        }
        if (events_.size() == events_.max_size()) {
            return std::unexpected{SimulationQueueError::CapacityExceeded};
        }
        static_assert(std::is_nothrow_copy_constructible_v<SimulationEvent>);
        static_assert(std::is_nothrow_copy_assignable_v<SimulationEvent>);
        static_assert(std::is_nothrow_move_constructible_v<SimulationEvent>);
        static_assert(std::is_nothrow_move_assignable_v<SimulationEvent>);
        events_.push_back(SimulationEvent{*sequence, std::move(request)});
        std::push_heap(events_.begin(), events_.end(), LaterArrival{});
        last_sequence_ = *sequence;
        return *sequence;
    }

    [[nodiscard]] std::optional<SimulationEvent> pop_next() noexcept {
        if (events_.empty()) {
            return std::nullopt;
        }
        std::pop_heap(events_.begin(), events_.end(), LaterArrival{});
        auto event = std::move(events_.back());
        events_.pop_back();
        return event;
    }

private:
    struct LaterArrival {
        [[nodiscard]] bool operator()(const SimulationEvent& left, const SimulationEvent& right) const noexcept {
            // Stored variants cannot become valueless: all payload copy/move operations are nonthrowing.
            return scheduled_before(right, left);
        }
    };

    std::vector<SimulationEvent> events_;
    SimulationSequence last_sequence_{};
};

} // namespace marketlab
