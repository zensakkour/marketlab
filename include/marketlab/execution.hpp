#pragma once

#include <marketlab/identifiers.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/side.hpp>
#include <marketlab/time.hpp>

namespace marketlab {

struct Execution {
    LogicalTime timestamp;
    EventSequence event_sequence;
    RequestSequence request_sequence;
    InstrumentId instrument_id;
    OrderId aggressive_order_id;
    OrderId resting_order_id;
    Side aggressive_side;
    ParticipantId buyer_id;
    ParticipantId seller_id;
    Price price;
    Quantity quantity;
};

// Matching against authoritative orders establishes price, priority, and counterparty
// correctness. Equal buyer/seller IDs are permitted by the v0 self-match policy.
[[nodiscard]] constexpr bool is_valid(const Execution& execution) noexcept {
    return is_valid(execution.event_sequence) && is_valid(execution.request_sequence) &&
           is_valid(execution.instrument_id) && is_valid(execution.aggressive_order_id) &&
           is_valid(execution.resting_order_id) &&
           execution.aggressive_order_id != execution.resting_order_id &&
           is_valid(execution.aggressive_side) && is_valid(execution.buyer_id) &&
           is_valid(execution.seller_id) && is_valid_order_quantity(execution.quantity);
}

} // namespace marketlab
