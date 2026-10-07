#include <marketlab/events.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

using marketlab::AcceptedOrder;
using marketlab::CancelRejected;
using marketlab::CancelRequest;
using marketlab::EventHeader;
using marketlab::ExchangeEvent;
using marketlab::Execution;
using marketlab::MarketRemainderExpired;
using marketlab::OrderAccepted;
using marketlab::OrderCancelled;
using marketlab::OrderFilled;
using marketlab::OrderId;
using marketlab::OrderPartiallyFilled;
using marketlab::OrderRejected;
using marketlab::OrderRequest;
using marketlab::OrderRested;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RejectionReason;
using marketlab::Side;
using marketlab::is_valid;

constexpr auto zero = Quantity::from_units(0).value();
constexpr auto five = Quantity::from_units(5).value();
constexpr auto price = Price::from_ticks(101).value();
constexpr EventHeader header{{5}, {1}, {1}, {1}};
constexpr AcceptedOrder order{
    .order_id = {3},
    .participant_id = {2},
    .instrument_id = {1},
    .side = Side::Buy,
    .type = OrderType::Limit,
    .original_quantity = five,
    .limit_price = price,
    .accepted_at = {5},
    .arrival_sequence = {1},
};
constexpr Execution execution{
    .timestamp = {5},
    .event_sequence = {2},
    .request_sequence = {1},
    .instrument_id = {1},
    .aggressive_order_id = {3},
    .resting_order_id = {4},
    .aggressive_side = Side::Buy,
    .buyer_id = {2},
    .seller_id = {6},
    .price = price,
    .quantity = five,
};
constexpr OrderRequest request{
    .timestamp = {5},
    .instrument_id = {1},
    .participant_id = {2},
    .order_id = {3},
    .side = Side::Buy,
    .type = OrderType::Limit,
    .quantity_units = 5,
    .limit_price_ticks = 101,
};
constexpr CancelRequest cancel{{5}, {1}, {2}, {3}};

static_assert(std::variant_size_v<ExchangeEvent> == 9);
static_assert(!std::is_default_constructible_v<ExchangeEvent>);
static_assert(is_valid(ExchangeEvent{OrderAccepted{header, order}}));
static_assert(is_valid(OrderFilled{header, {3}, {2}, zero}));
static_assert(!is_valid(OrderPartiallyFilled{header, {3}, {2}, zero}));

int main() {
    // Independent record shapes, not an emitted stream or an ordering test.
    const std::array<ExchangeEvent, 9> records{
        OrderAccepted{header, order},
        execution,
        OrderPartiallyFilled{header, {3}, {2}, five},
        OrderFilled{header, {3}, {2}, zero},
        OrderRested{header, {3}, {2}, price, five},
        MarketRemainderExpired{header, {3}, {2}, five},
        OrderCancelled{header, {3}, {2}, price, five},
        OrderRejected{header, request, RejectionReason::DuplicateOrderId},
        CancelRejected{header, cancel, RejectionReason::NotActive},
    };
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (records[index].index() != index || !is_valid(records[index])) {
            std::cerr << "valid event shape or variant alternative lost\n";
            return 1;
        }
        for (int sequence = 0; sequence < 2; ++sequence) {
            auto invalid = records[index];
            std::visit([sequence](auto& record) {
                if constexpr (std::is_same_v<std::decay_t<decltype(record)>, Execution>) {
                    if (sequence == 0) {
                        record.event_sequence = {};
                    } else {
                        record.request_sequence = {};
                    }
                } else {
                    if (sequence == 0) {
                        record.header.event_sequence = {};
                    } else {
                        record.header.request_sequence = {};
                    }
                }
            }, invalid);
            if (is_valid(invalid)) {
                std::cerr << "event allowed an unassigned sequence\n";
                return 1;
            }
        }
    }
    const auto& recorded_execution = std::get<Execution>(records[1]);
    if (recorded_execution.price != price || recorded_execution.quantity != five ||
        recorded_execution.buyer_id != ParticipantId{2} ||
        recorded_execution.seller_id != ParticipantId{6}) {
        std::cerr << "execution payload lost in event variant\n";
        return 1;
    }

    constexpr auto maximum_units = std::numeric_limits<std::int64_t>::max();
    for (const auto units : std::array<std::int64_t, 3>{0, 1, maximum_units}) {
        const auto quantity = Quantity::from_units(units).value();
        if (is_valid(OrderPartiallyFilled{header, {3}, {2}, quantity}) != (units > 0) ||
            is_valid(OrderFilled{header, {3}, {2}, quantity}) != (units == 0) ||
            is_valid(OrderRested{header, {3}, {2}, price, quantity}) != (units > 0) ||
            is_valid(MarketRemainderExpired{header, {3}, {2}, quantity}) != (units > 0) ||
            is_valid(OrderCancelled{header, {3}, {2}, price, quantity}) != (units > 0)) {
            std::cerr << "event quantity contradicts fill/rest/expiry/cancel status\n";
            return 1;
        }
    }
    for (const auto invalid_header : std::array{
             EventHeader{{5}, {0}, {1}, {1}},
             EventHeader{{5}, {1}, {0}, {1}},
             EventHeader{{5}, {1}, {1}, {0}},
         }) {
        if (is_valid(OrderAccepted{invalid_header, order}) ||
            is_valid(OrderPartiallyFilled{invalid_header, {3}, {2}, five}) ||
            is_valid(OrderFilled{invalid_header, {3}, {2}, zero}) ||
            is_valid(OrderRested{invalid_header, {3}, {2}, price, five}) ||
            is_valid(MarketRemainderExpired{invalid_header, {3}, {2}, five}) ||
            is_valid(OrderCancelled{invalid_header, {3}, {2}, price, five})) {
            std::cerr << "successful event allowed invalid header\n";
            return 1;
        }
    }
    for (const auto ids : std::array{
             std::pair{OrderId{}, ParticipantId{2}},
             std::pair{OrderId{3}, ParticipantId{}},
         }) {
        if (is_valid(OrderPartiallyFilled{header, ids.first, ids.second, five}) ||
            is_valid(OrderFilled{header, ids.first, ids.second, zero}) ||
            is_valid(OrderRested{header, ids.first, ids.second, price, five}) ||
            is_valid(MarketRemainderExpired{header, ids.first, ids.second, five}) ||
            is_valid(OrderCancelled{header, ids.first, ids.second, price, five})) {
            std::cerr << "successful event allowed invalid order or owner ID\n";
            return 1;
        }
    }
    for (const auto mismatch : std::array{
             EventHeader{{6}, {1}, {1}, {1}},
             EventHeader{{5}, {1}, {2}, {1}},
             EventHeader{{5}, {1}, {1}, {2}},
         }) {
        if (is_valid(OrderAccepted{mismatch, order})) {
            std::cerr << "acceptance header disagrees with accepted order\n";
            return 1;
        }
    }

    struct ReasonCase {
        RejectionReason reason;
        bool order_valid;
        bool cancel_valid;
    };
    constexpr std::array reasons{
        ReasonCase{RejectionReason::InvalidInstrument, true, true},
        ReasonCase{RejectionReason::InvalidParticipant, true, true},
        ReasonCase{RejectionReason::InvalidOrderId, true, true},
        ReasonCase{RejectionReason::InvalidSide, true, false},
        ReasonCase{RejectionReason::InvalidOrderType, true, false},
        ReasonCase{RejectionReason::InvalidQuantity, true, false},
        ReasonCase{RejectionReason::InvalidPrice, true, false},
        ReasonCase{RejectionReason::DuplicateOrderId, true, false},
        ReasonCase{RejectionReason::NotActive, false, true},
        ReasonCase{RejectionReason::NotOwner, false, true},
        ReasonCase{static_cast<RejectionReason>(-1), false, false},
        ReasonCase{static_cast<RejectionReason>(99), false, false},
    };
    for (const auto& test : reasons) {
        if (is_valid(OrderRejected{header, request, test.reason}) != test.order_valid ||
            is_valid(CancelRejected{header, cancel, test.reason}) != test.cancel_valid) {
            std::cerr << "rejection reason does not belong to request kind\n";
            return 1;
        }
    }
    for (const auto time : std::array<std::uint64_t, 3>{4, 5, 6}) {
        auto submitted_order = request;
        auto submitted_cancel = cancel;
        submitted_order.timestamp = {time};
        submitted_cancel.timestamp = {time};
        if (is_valid(OrderRejected{header, submitted_order, RejectionReason::InvalidTimestamp}) !=
                (time < 5) ||
            is_valid(CancelRejected{header, submitted_cancel, RejectionReason::InvalidTimestamp}) !=
                (time < 5) ||
            is_valid(OrderRejected{header, submitted_order, RejectionReason::InvalidPrice}) !=
                (time == 5) ||
            is_valid(CancelRejected{header, submitted_cancel, RejectionReason::NotActive}) !=
                (time == 5)) {
            std::cerr << "rejection timestamp does not follow logical-clock contract\n";
            return 1;
        }
    }
    auto raw_request = OrderRequest{};
    raw_request.quantity_units = std::numeric_limits<std::int64_t>::min();
    raw_request.limit_price_ticks = 0;
    const EventHeader raw_header{{0}, {1}, {1}, {0}};
    const ExchangeEvent rejected = OrderRejected{raw_header, raw_request,
                                                 RejectionReason::InvalidInstrument};
    const auto& stored = std::get<OrderRejected>(rejected).request;
    if (!is_valid(rejected) || stored.order_id != OrderId{} ||
        stored.participant_id != ParticipantId{} || stored.side != Side{} ||
        stored.quantity_units != raw_request.quantity_units || stored.limit_price_ticks != 0 ||
        !is_valid(CancelRejected{raw_header, CancelRequest{}, RejectionReason::InvalidInstrument})) {
        std::cerr << "rejection failed to preserve malformed submitted fields\n";
        return 1;
    }
    auto mismatch = header;
    mismatch.instrument_id = {2};
    if (is_valid(OrderRejected{mismatch, request, RejectionReason::InvalidPrice}) ||
        is_valid(CancelRejected{mismatch, cancel, RejectionReason::NotActive})) {
        std::cerr << "rejection instrument differs from submitted request\n";
        return 1;
    }
    constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
    const EventHeader maximum_header{
        {maximum_sequence}, {maximum_sequence}, {maximum_sequence}, {1}};
    if (!is_valid(OrderFilled{maximum_header, {3}, {2}, zero})) {
        std::cerr << "maximum event sequences or timestamp rejected\n";
        return 1;
    }
}
