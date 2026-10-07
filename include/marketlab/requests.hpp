#pragma once

#include <marketlab/identifiers.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/side.hpp>
#include <marketlab/time.hpp>

#include <cstdint>
#include <optional>

namespace marketlab {

enum class OrderType : std::uint8_t {
    Limit = 1,
    Market = 2,
};

enum class RejectionReason {
    InvalidTimestamp,
    InvalidInstrument,
    InvalidParticipant,
    InvalidOrderId,
    InvalidSide,
    InvalidOrderType,
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    NotActive,
    NotOwner,
};

struct OrderRequest {
    LogicalTime timestamp{};
    InstrumentId instrument_id{};
    ParticipantId participant_id{};
    OrderId order_id{};
    Side side{};
    OrderType type{};
    // Preserve unvalidated numbers so the exchange can reject the submitted values.
    std::int64_t quantity_units{};
    std::optional<std::int64_t> limit_price_ticks;
};

struct CancelRequest {
    LogicalTime timestamp{};
    InstrumentId instrument_id{};
    ParticipantId participant_id{};
    OrderId target_order_id{};
};

// These checks do not consult exchange state. Clock ordering and configured-ID
// checks must precede them; duplicate/active-order/ownership checks follow them.
[[nodiscard]] constexpr std::optional<RejectionReason> validate_fields(
    const OrderRequest& request) noexcept {
    if (!is_valid(request.instrument_id)) {
        return RejectionReason::InvalidInstrument;
    }
    if (!is_valid(request.participant_id)) {
        return RejectionReason::InvalidParticipant;
    }
    if (!is_valid(request.order_id)) {
        return RejectionReason::InvalidOrderId;
    }
    if (!is_valid(request.side)) {
        return RejectionReason::InvalidSide;
    }
    if (request.type != OrderType::Limit && request.type != OrderType::Market) {
        return RejectionReason::InvalidOrderType;
    }
    const auto quantity = Quantity::from_units(request.quantity_units);
    if (!quantity || !is_valid_order_quantity(*quantity)) {
        return RejectionReason::InvalidQuantity;
    }
    if (request.type == OrderType::Limit) {
        if (!request.limit_price_ticks || !Price::from_ticks(*request.limit_price_ticks)) {
            return RejectionReason::InvalidPrice;
        }
    } else if (request.limit_price_ticks) {
        return RejectionReason::InvalidPrice;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<RejectionReason> validate_fields(
    const CancelRequest& request) noexcept {
    if (!is_valid(request.instrument_id)) {
        return RejectionReason::InvalidInstrument;
    }
    if (!is_valid(request.participant_id)) {
        return RejectionReason::InvalidParticipant;
    }
    if (!is_valid(request.target_order_id)) {
        return RejectionReason::InvalidOrderId;
    }
    return std::nullopt;
}

} // namespace marketlab
