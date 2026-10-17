#pragma once

#include <marketlab/participant_plans.hpp>
#include <marketlab/scripted_participant.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace participant_test {

using namespace marketlab;
inline constexpr std::array participants{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}};

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

inline bool same_request(const SimulationRequest& left, const SimulationRequest& right) {
    return std::visit([&](const auto& request) {
        using Request = std::decay_t<decltype(request)>;
        const auto* other = std::get_if<Request>(&right);
        if (!other || request.timestamp != other->timestamp || request.instrument_id != other->instrument_id ||
            request.participant_id != other->participant_id) return false;
        if constexpr (std::is_same_v<Request, OrderRequest>) {
            return request.order_id == other->order_id && request.side == other->side && request.type == other->type &&
                request.quantity_units == other->quantity_units && request.limit_price_ticks == other->limit_price_ticks;
        } else {
            return request.target_order_id == other->target_order_id;
        }
    }, left);
}

inline bool same_plan(std::span<const SimulationRequest> left, std::span<const SimulationRequest> right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), same_request);
}

inline Simulation make_simulation() {
    return Simulation{Instrument{{1}, "TEST", TickSize::from_decimal(1, 2).value()}, participants};
}

inline OrderFlowConfig flow() {
    return {{1}, {2}, {10}, 20, 1, 5, 1, 4, 5000};
}

inline NoiseConfig noise() {
    return {flow(), Price::from_ticks(100).value(), 4, 3000};
}

inline LiquidityProviderConfig provider() {
    return {{1}, {1}, Price::from_ticks(100).value(), 2, 5, {0}, 10, 3};
}

inline bool same_random(const SimulationRandom& left, const SimulationRandom& right) {
    auto first = left;
    auto second = right;
    for (int index = 0; index < 10; ++index) {
        if (first.uniform(0, UINT64_MAX) != second.uniform(0, UINT64_MAX)) return false;
    }
    return true;
}

} // namespace participant_test
