#pragma once

#include <marketlab/events.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace marketlab {

namespace detail {

inline void write_record_text(std::ostream& output, std::string_view text) {
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

inline void write_record_integer(std::ostream& output, std::string_view field, auto value) {
    static_assert(std::numeric_limits<decltype(value)>::is_integer &&
        std::numeric_limits<decltype(value)>::digits <= 64);
    // Both signed and unsigned 64-bit decimal values fit, including the minus sign.
    char digits[32];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    write_record_text(output, " ");
    write_record_text(output, field);
    write_record_text(output, "=");
    output.write(digits, result.ptr - digits);
}

inline void write_record_limit(std::ostream& output, std::optional<std::int64_t> ticks) {
    if (ticks) {
        write_record_integer(output, "limit_ticks", *ticks);
    } else {
        write_record_text(output, " limit_ticks=none");
    }
}

[[nodiscard]] constexpr std::string_view record_rejection_name(RejectionReason reason) noexcept {
    switch (reason) {
    case RejectionReason::InvalidTimestamp: return "InvalidTimestamp";
    case RejectionReason::InvalidInstrument: return "InvalidInstrument";
    case RejectionReason::InvalidParticipant: return "InvalidParticipant";
    case RejectionReason::InvalidOrderId: return "InvalidOrderId";
    case RejectionReason::InvalidSide: return "InvalidSide";
    case RejectionReason::InvalidOrderType: return "InvalidOrderType";
    case RejectionReason::InvalidQuantity: return "InvalidQuantity";
    case RejectionReason::InvalidPrice: return "InvalidPrice";
    case RejectionReason::DuplicateOrderId: return "DuplicateOrderId";
    case RejectionReason::NotActive: return "NotActive";
    case RejectionReason::NotOwner: return "NotOwner";
    }
    return "Unknown";
}

} // namespace detail

// Observe an already produced event; stream failures never reprocess an arrival.
inline void write_event_record(std::ostream& output, const ExchangeEvent& event) {
    if (!is_valid(event)) {
        throw std::invalid_argument{"event recording requires an intrinsically valid exchange event"};
    }
    std::visit([&](const auto& record) {
        using Record = std::decay_t<decltype(record)>;
        constexpr std::string_view name = [] {
            if constexpr (std::is_same_v<Record, OrderAccepted>) return "OrderAccepted";
            else if constexpr (std::is_same_v<Record, Execution>) return "Execution";
            else if constexpr (std::is_same_v<Record, OrderPartiallyFilled>) return "OrderPartiallyFilled";
            else if constexpr (std::is_same_v<Record, OrderFilled>) return "OrderFilled";
            else if constexpr (std::is_same_v<Record, OrderRested>) return "OrderRested";
            else if constexpr (std::is_same_v<Record, MarketRemainderExpired>) return "MarketRemainderExpired";
            else if constexpr (std::is_same_v<Record, OrderCancelled>) return "OrderCancelled";
            else if constexpr (std::is_same_v<Record, OrderRejected>) return "OrderRejected";
            else {
                static_assert(std::is_same_v<Record, CancelRejected>, "event-record schema must cover every event type");
                return "CancelRejected";
            }
        }();
        const EventHeader header = [&] {
            if constexpr (std::is_same_v<Record, Execution>) {
                return EventHeader{record.timestamp, record.event_sequence, record.request_sequence, record.instrument_id};
            } else {
                return record.header;
            }
        }();
        detail::write_record_text(output, "v=1 type=");
        detail::write_record_text(output, name);
        detail::write_record_integer(output, "time_ns", header.timestamp.nanoseconds);
        detail::write_record_integer(output, "event_seq", header.event_sequence.value);
        detail::write_record_integer(output, "request_seq", header.request_sequence.value);
        detail::write_record_integer(output, "instrument_id", header.instrument_id.value);
        if constexpr (std::is_same_v<Record, OrderAccepted>) {
            const auto& order = record.order;
            detail::write_record_integer(output, "order_id", order.order_id.value);
            detail::write_record_integer(output, "participant_id", order.participant_id.value);
            detail::write_record_integer(output, "side", static_cast<unsigned>(order.side));
            detail::write_record_integer(output, "order_type", static_cast<unsigned>(order.type));
            detail::write_record_integer(output, "original_units", order.original_quantity.units());
            detail::write_record_limit(output, order.limit_price ? std::optional{order.limit_price->ticks()} : std::nullopt);
        } else if constexpr (std::is_same_v<Record, Execution>) {
            detail::write_record_integer(output, "aggressive_order_id", record.aggressive_order_id.value);
            detail::write_record_integer(output, "resting_order_id", record.resting_order_id.value);
            detail::write_record_integer(output, "aggressive_side", static_cast<unsigned>(record.aggressive_side));
            detail::write_record_integer(output, "buyer_id", record.buyer_id.value);
            detail::write_record_integer(output, "seller_id", record.seller_id.value);
            detail::write_record_integer(output, "price_ticks", record.price.ticks());
            detail::write_record_integer(output, "quantity_units", record.quantity.units());
        } else if constexpr (std::is_same_v<Record, OrderRejected> || std::is_same_v<Record, CancelRejected>) {
            const auto& request = record.request;
            detail::write_record_integer(output, "submitted_ns", request.timestamp.nanoseconds);
            detail::write_record_integer(output, "participant_id", request.participant_id.value);
            if constexpr (std::is_same_v<Record, OrderRejected>) {
                detail::write_record_integer(output, "order_id", request.order_id.value);
                detail::write_record_integer(output, "side", static_cast<unsigned>(request.side));
                detail::write_record_integer(output, "order_type", static_cast<unsigned>(request.type));
                detail::write_record_integer(output, "quantity_units", request.quantity_units);
                detail::write_record_limit(output, request.limit_price_ticks);
            } else {
                detail::write_record_integer(output, "target_order_id", request.target_order_id.value);
            }
            detail::write_record_text(output, " reason=");
            detail::write_record_text(output, detail::record_rejection_name(record.reason));
        } else {
            detail::write_record_integer(output, "order_id", record.order_id.value);
            detail::write_record_integer(output, "participant_id", record.participant_id.value);
            if constexpr (std::is_same_v<Record, OrderCancelled>) {
                detail::write_record_integer(output, "price_ticks", record.price.ticks());
                detail::write_record_integer(output, "cancelled_units", record.cancelled_quantity.units());
            } else {
                if constexpr (std::is_same_v<Record, OrderRested>) {
                    detail::write_record_integer(output, "price_ticks", record.price.ticks());
                }
                detail::write_record_integer(output, "remaining_units", record.remaining_quantity.units());
            }
        }
    }, event);
    output.put('\n');
}

} // namespace marketlab
