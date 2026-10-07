#pragma once

#include <marketlab/identifiers.hpp>
#include <marketlab/requests.hpp>
#include <marketlab/time.hpp>

#include <utility>
#include <variant>

namespace marketlab {

using SimulationRequest = std::variant<OrderRequest, CancelRequest>;

struct SimulationEvent {
    SimulationSequence sequence;
    // The request timestamp is its scheduled exchange-arrival time.
    SimulationRequest request;
};

// Scheduling preserves malformed requests for rejection by the exchange.
[[nodiscard]] constexpr bool is_valid(const SimulationEvent& event) noexcept {
    return is_valid(event.sequence) && !event.request.valueless_by_exception();
}

[[nodiscard]] constexpr LogicalTime scheduled_time(const SimulationEvent& event) {
    return std::visit([](const auto& request) { return request.timestamp; }, event.request);
}

// A future scheduler assigns sequences in insertion order, independently of the
// exchange's processing sequences. Earlier time takes precedence over that tie-break.
[[nodiscard]] constexpr bool scheduled_before(
    const SimulationEvent& left, const SimulationEvent& right) {
    return std::pair{scheduled_time(left), left.sequence} <
           std::pair{scheduled_time(right), right.sequence};
}

} // namespace marketlab
