#pragma once

#include <marketlab/identifiers.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/requests.hpp>
#include <marketlab/side.hpp>
#include <marketlab/time.hpp>

#include <optional>

namespace marketlab {

struct AcceptedOrder {
    OrderId order_id;
    ParticipantId participant_id;
    InstrumentId instrument_id;
    Side side;
    OrderType type;
    Quantity original_quantity;
    std::optional<Price> limit_price;
    LogicalTime accepted_at;
    RequestSequence arrival_sequence;
};

struct RestingOrder {
    AcceptedOrder order;
    Quantity remaining_quantity;
};

// Intrinsic consistency does not establish acceptance, registration, or book membership.
[[nodiscard]] constexpr bool is_valid(const AcceptedOrder& order) noexcept {
    return is_valid(order.order_id) && is_valid(order.participant_id) &&
           is_valid(order.instrument_id) && is_valid(order.side) &&
           is_valid_order_quantity(order.original_quantity) && is_valid(order.arrival_sequence) &&
           ((order.type == OrderType::Limit && order.limit_price.has_value()) ||
            (order.type == OrderType::Market && !order.limit_price.has_value()));
}

[[nodiscard]] constexpr bool is_valid(const RestingOrder& resting) noexcept {
    return is_valid(resting.order) && resting.order.type == OrderType::Limit &&
           is_valid_order_quantity(resting.remaining_quantity) &&
           resting.remaining_quantity <= resting.order.original_quantity;
}

} // namespace marketlab
