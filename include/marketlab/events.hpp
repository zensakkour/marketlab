#pragma once

#include <marketlab/execution.hpp>
#include <marketlab/identifiers.hpp>
#include <marketlab/orders.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/requests.hpp>
#include <marketlab/time.hpp>

#include <variant>

namespace marketlab {

struct EventHeader {
    LogicalTime timestamp;
    EventSequence event_sequence;
    RequestSequence request_sequence;
    InstrumentId instrument_id;
};

// Rejections may carry a zero or unknown submitted instrument ID.
[[nodiscard]] constexpr bool is_valid(const EventHeader& header) noexcept {
    return is_valid(header.event_sequence) && is_valid(header.request_sequence);
}

struct OrderAccepted {
    EventHeader header;
    AcceptedOrder order;
};

struct OrderPartiallyFilled {
    EventHeader header;
    OrderId order_id;
    ParticipantId participant_id;
    Quantity remaining_quantity;
};

struct OrderFilled {
    EventHeader header;
    OrderId order_id;
    ParticipantId participant_id;
    Quantity remaining_quantity;
};

struct OrderRested {
    EventHeader header;
    OrderId order_id;
    ParticipantId participant_id;
    Price price;
    Quantity remaining_quantity;
};

struct MarketRemainderExpired {
    EventHeader header;
    OrderId order_id;
    ParticipantId participant_id;
    Quantity remaining_quantity;
};

struct OrderCancelled {
    EventHeader header;
    OrderId order_id;
    ParticipantId participant_id;
    Price price;
    Quantity cancelled_quantity;
};

struct OrderRejected {
    EventHeader header;
    OrderRequest request;
    RejectionReason reason;
};

struct CancelRejected {
    EventHeader header;
    CancelRequest request;
    RejectionReason reason;
};

[[nodiscard]] constexpr bool is_valid(const OrderAccepted& event) noexcept {
    return is_valid(event.header) && is_valid(event.order) &&
           event.header.instrument_id == event.order.instrument_id &&
           event.header.timestamp == event.order.accepted_at &&
           event.header.request_sequence == event.order.arrival_sequence;
}

[[nodiscard]] constexpr bool is_valid(const OrderPartiallyFilled& event) noexcept {
    return is_valid(event.header) && is_valid(event.header.instrument_id) &&
           is_valid(event.order_id) && is_valid(event.participant_id) &&
           is_valid_order_quantity(event.remaining_quantity);
}

[[nodiscard]] constexpr bool is_valid(const OrderFilled& event) noexcept {
    return is_valid(event.header) && is_valid(event.header.instrument_id) &&
           is_valid(event.order_id) && is_valid(event.participant_id) &&
           event.remaining_quantity.units() == 0;
}

[[nodiscard]] constexpr bool is_valid(const OrderRested& event) noexcept {
    return is_valid(event.header) && is_valid(event.header.instrument_id) &&
           is_valid(event.order_id) && is_valid(event.participant_id) &&
           is_valid_order_quantity(event.remaining_quantity);
}

[[nodiscard]] constexpr bool is_valid(const MarketRemainderExpired& event) noexcept {
    return is_valid(event.header) && is_valid(event.header.instrument_id) &&
           is_valid(event.order_id) && is_valid(event.participant_id) &&
           is_valid_order_quantity(event.remaining_quantity);
}

[[nodiscard]] constexpr bool is_valid(const OrderCancelled& event) noexcept {
    return is_valid(event.header) && is_valid(event.header.instrument_id) &&
           is_valid(event.order_id) && is_valid(event.participant_id) &&
           is_valid_order_quantity(event.cancelled_quantity);
}

// These checks establish record shape, not the exchange's reason selection or
// event ordering. Rejected requests retain their unvalidated submitted fields.
[[nodiscard]] constexpr bool is_valid(const OrderRejected& event) noexcept {
    return is_valid(event.header) &&
           event.header.instrument_id == event.request.instrument_id &&
           is_valid(event.reason) && event.reason != RejectionReason::NotActive &&
           event.reason != RejectionReason::NotOwner &&
           (event.reason == RejectionReason::InvalidTimestamp
                ? event.request.timestamp < event.header.timestamp
                : event.request.timestamp == event.header.timestamp);
}

[[nodiscard]] constexpr bool is_valid(const CancelRejected& event) noexcept {
    switch (event.reason) {
    case RejectionReason::InvalidTimestamp:
    case RejectionReason::InvalidInstrument:
    case RejectionReason::InvalidParticipant:
    case RejectionReason::InvalidOrderId:
    case RejectionReason::NotActive:
    case RejectionReason::NotOwner:
        break;
    default:
        return false;
    }
    return is_valid(event.header) &&
           event.header.instrument_id == event.request.instrument_id &&
           (event.reason == RejectionReason::InvalidTimestamp
                ? event.request.timestamp < event.header.timestamp
                : event.request.timestamp == event.header.timestamp);
}

using ExchangeEvent = std::variant<OrderAccepted, Execution, OrderPartiallyFilled,
                                   OrderFilled, OrderRested, MarketRemainderExpired,
                                   OrderCancelled, OrderRejected, CancelRejected>;

[[nodiscard]] constexpr bool is_valid(const ExchangeEvent& event) {
    if (event.valueless_by_exception()) {
        return false;
    }
    return std::visit([](const auto& record) { return is_valid(record); }, event);
}

} // namespace marketlab
