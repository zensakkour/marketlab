#include <marketlab/exchange.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using marketlab::CancelRejected;
using marketlab::CancelRequest;
using marketlab::OrderCancelled;
using marketlab::EventHeader;
using marketlab::Exchange;
using marketlab::ExchangeEvent;
using marketlab::Execution;
using marketlab::Instrument;
using marketlab::OrderAccepted;
using marketlab::OrderFilled;
using marketlab::OrderPartiallyFilled;
using marketlab::OrderRejected;
using marketlab::OrderRequest;
using marketlab::OrderRested;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::RejectionReason;
using marketlab::Side;
using marketlab::TickSize;

constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
constexpr auto maximum_units = std::numeric_limits<std::int64_t>::max();
constexpr std::array participants{ParticipantId{1}, ParticipantId{2}};
constexpr auto tick = TickSize::from_decimal(1, 2).value();

static_assert(!std::is_copy_constructible_v<Exchange> && !std::is_move_constructible_v<Exchange>);
static_assert(std::is_same_v<decltype(std::declval<Exchange&>().book()), const marketlab::OrderBook&>);
static_assert(marketlab::detail::next_sequence_value(0) == 1);
static_assert(marketlab::detail::next_sequence_value(maximum_sequence - 1) == maximum_sequence);
static_assert(!marketlab::detail::next_sequence_value(maximum_sequence));

OrderRequest make_order(std::uint64_t id, Side side, std::int64_t price = 100,
    std::int64_t units = 5, std::uint64_t time = 0, std::uint64_t owner = 1) {
    return {{time}, {1}, {owner}, {id}, side, OrderType::Limit, units, price};
}

bool same_request(const OrderRequest& left, const OrderRequest& right) {
    return left.timestamp == right.timestamp && left.instrument_id == right.instrument_id &&
        left.participant_id == right.participant_id && left.order_id == right.order_id &&
        left.side == right.side && left.type == right.type && left.quantity_units == right.quantity_units &&
        left.limit_price_ticks == right.limit_price_ticks;
}

bool valid_stream(const std::vector<ExchangeEvent>& events, const OrderRequest& request,
    std::uint64_t sequence, std::uint64_t first_event, std::uint64_t time) {
    if (events.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (!marketlab::is_valid(events[index])) {
            return false;
        }
        const auto header = std::visit([](const auto& event) {
            if constexpr (std::is_same_v<std::decay_t<decltype(event)>, Execution>) {
                return EventHeader{event.timestamp, event.event_sequence, event.request_sequence, event.instrument_id};
            } else {
                return event.header;
            }
        }, events[index]);
        if (header.timestamp.nanoseconds != time || header.instrument_id != request.instrument_id ||
            header.request_sequence.value != sequence || header.event_sequence.value != first_event + index) {
            return false;
        }
    }
    if (const auto* accepted = std::get_if<OrderAccepted>(&events.front())) {
        const auto& order = accepted->order;
        return order.order_id == request.order_id && order.participant_id == request.participant_id &&
            order.instrument_id == request.instrument_id && order.side == request.side && order.type == request.type &&
            order.original_quantity.units() == request.quantity_units && order.accepted_at == request.timestamp &&
            order.arrival_sequence.value == sequence &&
            (order.limit_price ? std::optional<std::int64_t>{order.limit_price->ticks()} : std::nullopt) == request.limit_price_ticks;
    }
    return true;
}

bool rejected(const std::vector<ExchangeEvent>& events, const OrderRequest& request, RejectionReason reason) {
    if (events.size() != 1) {
        return false;
    }
    const auto* rejection = std::get_if<OrderRejected>(&events.front());
    return rejection && rejection->reason == reason && same_request(rejection->request, request);
}

bool test_configuration() {
    for (int field = 0; field < 3; ++field) {
        auto instrument = Instrument{{1}, "XYZ", tick};
        auto ids = participants;
        if (field == 0) instrument.id = {};
        if (field == 1) instrument.symbol.clear();
        if (field == 2) ids[0] = {};
        try {
            Exchange invalid{instrument, ids};
            return false;
        } catch (const std::invalid_argument&) {
        }
    }
    auto ids = std::array{ParticipantId{1}, ParticipantId{1}, ParticipantId{2}};
    Exchange exchange{Instrument{{1}, "XYZ", tick}, ids};
    ids.fill(ParticipantId{99});
    if (!exchange.book().empty() || exchange.current_time().nanoseconds != 0 ||
        exchange.last_request_sequence().value != 0 || exchange.last_event_sequence().value != 0 ||
        !exchange.process_order(make_order(1, Side::Buy))) {
        return false;
    }
    Exchange no_participants{Instrument{{1}, "XYZ", tick}, {}};
    const auto request = make_order(1, Side::Buy);
    const auto result = no_participants.process_order(request);
    return result && rejected(*result, request, RejectionReason::InvalidParticipant) && no_participants.book().empty();
}

bool test_matching_and_sequences() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        Exchange exchange{Instrument{{1}, "XYZ", tick}, participants};
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        for (const auto id : {30U, 1U}) {
            const auto request = make_order(id, opposite, 100, 3);
            const auto before_request = exchange.last_request_sequence().value;
            const auto before_event = exchange.last_event_sequence().value;
            const auto result = exchange.process_order(request);
            if (!result || result->size() != 2 || !std::holds_alternative<OrderRested>(result->back()) ||
                !valid_stream(*result, request, before_request + 1, before_event + 1, 0)) {
                return false;
            }
        }
        auto invalid = make_order(99, side, 100, 0);
        const auto refusal = exchange.process_order(invalid);
        if (!refusal || !rejected(*refusal, invalid, RejectionReason::InvalidQuantity) ||
            !valid_stream(*refusal, invalid, 3, 5, 0)) {
            return false;
        }
        auto incoming = make_order(99, side, 100, 5, 0, 2);
        const auto result = exchange.process_order(incoming);
        if (!result || result->size() != 7 || !valid_stream(*result, incoming, 4, 6, 0)) {
            return false;
        }
        const auto* first = std::get_if<Execution>(&(*result)[1]);
        const auto* second = std::get_if<Execution>(&(*result)[4]);
        const auto* partial = std::get_if<OrderPartiallyFilled>(&(*result)[5]);
        const auto* rest = exchange.book().find({1});
        if (!first || !second || first->resting_order_id.value != 30 || second->resting_order_id.value != 1 ||
            first->quantity.units() != 3 || second->quantity.units() != 2 || first->price.ticks() != 100 ||
            second->price.ticks() != 100 || !std::holds_alternative<OrderFilled>((*result)[2]) ||
            !std::holds_alternative<OrderPartiallyFilled>((*result)[3]) || !partial ||
            partial->remaining_quantity.units() != 1 || !std::holds_alternative<OrderFilled>((*result)[6]) ||
            !rest || rest->remaining_quantity.units() != 1 || rest->order.arrival_sequence.value != 2 ||
            exchange.book().order_count() != 1 || exchange.last_event_sequence().value != 12) {
            return false;
        }
        incoming.order_id = {100};
        incoming.type = OrderType::Market;
        incoming.limit_price_ticks.reset();
        incoming.quantity_units = 2;
        incoming.timestamp = {1};
        const auto market = exchange.process_order(incoming);
        if (!market || market->size() != 5 || !valid_stream(*market, incoming, 5, 13, 1) ||
            !std::holds_alternative<marketlab::MarketRemainderExpired>(market->back()) ||
            !exchange.book().empty() || exchange.current_time().nanoseconds != 1 ||
            exchange.last_request_sequence().value != 5 || exchange.last_event_sequence().value != 17) {
            return false;
        }
    }
    return true;
}

bool test_validation_precedence() {
    Exchange exchange{Instrument{{1}, "XYZ", tick}, participants};
    if (!exchange.process_order(make_order(1, Side::Sell, 100, 5, 10))) {
        return false;
    }
    constexpr std::array reasons{RejectionReason::InvalidTimestamp, RejectionReason::InvalidInstrument,
        RejectionReason::InvalidParticipant, RejectionReason::InvalidOrderId, RejectionReason::InvalidSide,
        RejectionReason::InvalidOrderType, RejectionReason::InvalidQuantity, RejectionReason::InvalidPrice,
        RejectionReason::DuplicateOrderId};
    for (std::size_t field = 0; field < reasons.size(); ++field) {
        auto request = make_order(1, Side::Buy, -1, -1, 20, 2);
        if (field > 6) request.quantity_units = 1;
        if (field > 7) request.limit_price_ticks = 100;
        switch (field) {
        case 0: request.timestamp = {9}; request.instrument_id = {}; break;
        case 1: request.instrument_id = {99}; request.participant_id = {}; break;
        case 2: request.participant_id = {99}; request.order_id = {}; break;
        case 3: request.order_id = {}; request.side = static_cast<Side>(255); break;
        case 4: request.side = static_cast<Side>(255); request.type = static_cast<OrderType>(255); break;
        case 5: request.type = static_cast<OrderType>(255); break;
        default: break;
        }
        const auto before_time = exchange.current_time();
        const auto next_request = exchange.last_request_sequence().value + 1;
        const auto next_event = exchange.last_event_sequence().value + 1;
        const auto result = exchange.process_order(request);
        const auto time = std::max(before_time, request.timestamp).nanoseconds;
        const auto* rest = exchange.book().find({1});
        if (!result || !rejected(*result, request, reasons[field]) ||
            !valid_stream(*result, request, next_request, next_event, time) || !rest ||
            rest->remaining_quantity.units() != 5 || rest->order.arrival_sequence.value != 1 ||
            exchange.book().order_count() != 1 || exchange.current_time().nanoseconds != time ||
            exchange.last_request_sequence().value != next_request || exchange.last_event_sequence().value != next_event) {
            return false;
        }
    }
    for (const int field : {0, 1, 2, 3}) {
        auto request = make_order(2, Side::Buy, 99, 1, 20, 2);
        if (field == 0) request.instrument_id = {};
        if (field == 1) request.participant_id = {};
        if (field == 2) request.limit_price_ticks.reset();
        if (field == 3) request.type = OrderType::Market;
        const auto expected = field == 0 ? RejectionReason::InvalidInstrument :
            field == 1 ? RejectionReason::InvalidParticipant : RejectionReason::InvalidPrice;
        const auto result = exchange.process_order(request);
        if (!result || !rejected(*result, request, expected)) {
            return false;
        }
    }
    const auto request = make_order(2, Side::Buy, 99, 1, 20, 2);
    const auto result = exchange.process_order(request);
    return result && std::holds_alternative<OrderAccepted>(result->front()) && exchange.book().find({2});
}

bool test_lifetime_ids_and_clock() {
    Exchange exchange{Instrument{{1}, "XYZ", tick}, participants};
    auto rejected_first = make_order(1, Side::Sell, 100, 0, 10);
    const auto invalid = exchange.process_order(rejected_first);
    if (!invalid || !rejected(*invalid, rejected_first, RejectionReason::InvalidQuantity)) {
        return false;
    }
    auto backward = make_order(1, Side::Sell, 100, 5, 9);
    const auto backward_result = exchange.process_order(backward);
    if (!backward_result || !rejected(*backward_result, backward, RejectionReason::InvalidTimestamp) ||
        !valid_stream(*backward_result, backward, 2, 2, 10) || exchange.current_time().nanoseconds != 10) {
        return false;
    }
    if (!exchange.process_order(make_order(1, Side::Sell, 100, 5, 10)) ||
        !exchange.process_order(make_order(2, Side::Buy, 100, 5, 10, 2)) || !exchange.book().empty()) {
        return false;
    }
    auto market = make_order(3, Side::Buy, 100, 1, 10);
    market.type = OrderType::Market;
    market.limit_price_ticks.reset();
    if (!exchange.process_order(market)) {
        return false;
    }
    for (const auto id : {1U, 2U, 3U}) {
        const auto duplicate = make_order(id, Side::Buy, 99, 1, 11, 2);
        const auto result = exchange.process_order(duplicate);
        if (!result || !rejected(*result, duplicate, RejectionReason::DuplicateOrderId) || !exchange.book().empty()) {
            return false;
        }
    }
    Exchange fresh{Instrument{{1}, "XYZ", tick}, participants};
    const auto reused = fresh.process_order(make_order(1, Side::Buy));
    return reused && std::holds_alternative<OrderAccepted>(reused->front());
}

bool test_numeric_boundaries() {
    const std::array ids{ParticipantId{maximum_sequence}};
    Exchange exchange{Instrument{{maximum_sequence}, "XYZ", tick}, ids};
    auto request = make_order(maximum_sequence, Side::Sell, maximum_units, maximum_units,
        maximum_sequence, maximum_sequence);
    request.instrument_id = {maximum_sequence};
    const auto limit = exchange.process_order(request);
    if (!limit || !valid_stream(*limit, request, 1, 1, maximum_sequence)) {
        return false;
    }
    auto market = request;
    market.order_id = {maximum_sequence - 1};
    market.side = Side::Buy;
    market.type = OrderType::Market;
    market.limit_price_ticks.reset();
    const auto result = exchange.process_order(market);
    if (!result || result->size() != 4 || !valid_stream(*result, market, 2, 3, maximum_sequence) ||
        !exchange.book().empty()) {
        return false;
    }
    auto backward = request;
    backward.timestamp = {};
    const auto refusal = exchange.process_order(backward);
    return refusal && rejected(*refusal, backward, RejectionReason::InvalidTimestamp) &&
        valid_stream(*refusal, backward, 3, 7, maximum_sequence);
}

bool valid_cancel_stream(const std::vector<ExchangeEvent>& events, const CancelRequest& request,
    std::uint64_t sequence, std::uint64_t event_sequence, std::uint64_t time) {
    if (events.size() != 1 || !marketlab::is_valid(events.front())) {
        return false;
    }
    const auto* cancelled = std::get_if<OrderCancelled>(&events.front());
    const auto* rejected = std::get_if<CancelRejected>(&events.front());
    const auto* header = cancelled ? &cancelled->header : rejected ? &rejected->header : nullptr;
    return header && header->timestamp.nanoseconds == time && header->instrument_id == request.instrument_id &&
        header->request_sequence.value == sequence && header->event_sequence.value == event_sequence;
}

bool cancel_rejected(const std::vector<ExchangeEvent>& events, const CancelRequest& request, RejectionReason reason) {
    if (events.size() != 1) {
        return false;
    }
    const auto* rejected = std::get_if<CancelRejected>(&events.front());
    return rejected && rejected->reason == reason && rejected->request.timestamp == request.timestamp &&
        rejected->request.instrument_id == request.instrument_id &&
        rejected->request.participant_id == request.participant_id && rejected->request.target_order_id == request.target_order_id;
}

bool test_cancellation_and_sequences() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        Exchange exchange{Instrument{{1}, "XYZ", tick}, participants};
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        const auto worse = side == Side::Buy ? 99 : 101;
        for (const auto id : {30U, 1U, 20U, 40U}) {
            const auto result = exchange.process_order(make_order(id, side, id == 40 ? worse : 100, 3));
            if (!result || !std::holds_alternative<OrderAccepted>(result->front())) {
                return false;
            }
        }
        const auto fill_request = make_order(50, opposite, 100, 1, 0, 2);
        const auto fill = exchange.process_order(fill_request);
        if (!fill || fill->size() != 4 || !valid_stream(*fill, fill_request, 5, 9, 0)) {
            return false;
        }
        std::size_t removed = 0;
        for (const auto id : {1U, 40U, 30U}) {
            const CancelRequest request{{1}, {1}, {1}, {id}};
            const auto next_request = exchange.last_request_sequence().value + 1;
            const auto next_event = exchange.last_event_sequence().value + 1;
            const auto result = exchange.process_cancel(request);
            const auto* cancelled = result ? std::get_if<OrderCancelled>(&result->front()) : nullptr;
            ++removed;
            if (!result || !valid_cancel_stream(*result, request, next_request, next_event, 1) || !cancelled ||
                cancelled->order_id != request.target_order_id || cancelled->participant_id != request.participant_id ||
                cancelled->price.ticks() != (id == 40 ? worse : 100) ||
                cancelled->cancelled_quantity.units() != (id == 30 ? 2 : 3) || exchange.book().find({id}) ||
                exchange.book().order_count() != 4 - removed || exchange.current_time().nanoseconds != 1 ||
                exchange.last_request_sequence().value != next_request || exchange.last_event_sequence().value != next_event) {
                return false;
            }
            const auto* survivor = exchange.book().find({20});
            if (!survivor || survivor->order.arrival_sequence.value != 3 || survivor->remaining_quantity.units() != 3 ||
                survivor->order.original_quantity.units() != 3) {
                return false;
            }
        }
        const auto next_fill_request = make_order(51, opposite, 100, 1, 1, 2);
        const auto next_fill = exchange.process_order(next_fill_request);
        if (!next_fill || next_fill->size() != 4 || !valid_stream(*next_fill, next_fill_request, 9, 16, 1) ||
            std::get<Execution>((*next_fill)[1]).resting_order_id.value != 20) {
            return false;
        }
        const CancelRequest last{{1}, {1}, {1}, {20}};
        const auto last_result = exchange.process_cancel(last);
        if (!last_result || !valid_cancel_stream(*last_result, last, 10, 20, 1) || !exchange.book().empty() ||
            std::get<OrderCancelled>(last_result->front()).cancelled_quantity.units() != 2 ||
            std::get<Execution>((*fill)[1]).quantity.units() != 1 ||
            std::get<Execution>((*fill)[1]).resting_order_id.value != 30) {
            return false;
        }
        auto expired = make_order(77, opposite, 100, 1, 1, 2);
        expired.type = OrderType::Market;
        expired.limit_price_ticks.reset();
        if (!exchange.process_order(expired)) {
            return false;
        }
        // All inactive states share NotActive, even when the requester is not the old owner.
        for (const auto id : {1U, 30U, 40U, 20U, 50U, 51U, 77U, 99U}) {
            const CancelRequest request{{1}, {1}, {2}, {id}};
            const auto next_request = exchange.last_request_sequence().value + 1;
            const auto next_event = exchange.last_event_sequence().value + 1;
            const auto result = exchange.process_cancel(request);
            if (!result || !cancel_rejected(*result, request, RejectionReason::NotActive) ||
                !valid_cancel_stream(*result, request, next_request, next_event, 1) || !exchange.book().empty()) {
                return false;
            }
        }
        for (const auto id : {1U, 30U, 40U, 20U}) {
            const auto request = make_order(id, side, 100, 1, 1, 2);
            const auto result = exchange.process_order(request);
            if (!result || !rejected(*result, request, RejectionReason::DuplicateOrderId)) {
                return false;
            }
        }
        const auto unused = exchange.process_order(make_order(99, side, 100, 1, 1));
        if (!unused || !std::holds_alternative<OrderAccepted>(unused->front())) {
            return false;
        }
    }
    return true;
}

bool test_cancel_validation_and_clock() {
    Exchange exchange{Instrument{{1}, "XYZ", tick}, participants};
    if (!exchange.process_order(make_order(1, Side::Buy, 100, 5, 10))) {
        return false;
    }
    struct Case {
        CancelRequest request;
        RejectionReason reason;
    };
    const std::array cases{
        Case{{{9}, {0}, {0}, {0}}, RejectionReason::InvalidTimestamp},
        Case{{{20}, {99}, {0}, {0}}, RejectionReason::InvalidInstrument},
        Case{{{20}, {0}, {1}, {1}}, RejectionReason::InvalidInstrument},
        Case{{{20}, {1}, {99}, {0}}, RejectionReason::InvalidParticipant},
        Case{{{20}, {1}, {0}, {1}}, RejectionReason::InvalidParticipant},
        Case{{{20}, {1}, {2}, {0}}, RejectionReason::InvalidOrderId},
        Case{{{20}, {1}, {2}, {99}}, RejectionReason::NotActive},
        Case{{{20}, {1}, {2}, {1}}, RejectionReason::NotOwner},
    };
    for (const auto& test : cases) {
        const auto before = exchange.book().find({1});
        const auto time = std::max(exchange.current_time(), test.request.timestamp).nanoseconds;
        const auto next_request = exchange.last_request_sequence().value + 1;
        const auto next_event = exchange.last_event_sequence().value + 1;
        const auto result = exchange.process_cancel(test.request);
        if (!result || !cancel_rejected(*result, test.request, test.reason) ||
            !valid_cancel_stream(*result, test.request, next_request, next_event, time) ||
            exchange.book().find({1}) != before || !before || before->remaining_quantity.units() != 5 ||
            before->order.arrival_sequence.value != 1 || exchange.book().order_count() != 1 ||
            exchange.current_time().nanoseconds != time || exchange.last_request_sequence().value != next_request ||
            exchange.last_event_sequence().value != next_event) {
            return false;
        }
    }
    const auto backward = make_order(2, Side::Sell, 100, 2, 19, 2);
    const auto refused_order = exchange.process_order(backward);
    if (!refused_order || !rejected(*refused_order, backward, RejectionReason::InvalidTimestamp)) {
        return false;
    }
    const auto partial = exchange.process_order(make_order(2, Side::Sell, 100, 2, 20, 2));
    if (!partial || !std::holds_alternative<OrderAccepted>(partial->front())) {
        return false;
    }
    const CancelRequest cancel{{20}, {1}, {1}, {1}};
    const auto next_request = exchange.last_request_sequence().value + 1;
    const auto next_event = exchange.last_event_sequence().value + 1;
    const auto result = exchange.process_cancel(cancel);
    return result && valid_cancel_stream(*result, cancel, next_request, next_event, 20) && exchange.book().empty() &&
        std::holds_alternative<OrderCancelled>(result->front()) &&
        std::get<OrderCancelled>(result->front()).cancelled_quantity.units() == 3;
}

bool test_cancel_numeric_boundaries() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        const std::array ids{ParticipantId{maximum_sequence}};
        Exchange exchange{Instrument{{maximum_sequence}, "XYZ", tick}, ids};
        auto order = make_order(maximum_sequence, side, maximum_units, maximum_units,
            maximum_sequence, maximum_sequence);
        order.instrument_id = {maximum_sequence};
        if (!exchange.process_order(order)) {
            return false;
        }
        const CancelRequest cancel{{maximum_sequence}, {maximum_sequence}, {maximum_sequence}, {maximum_sequence}};
        const auto result = exchange.process_cancel(cancel);
        const auto* event = result ? std::get_if<OrderCancelled>(&result->front()) : nullptr;
        if (!result || !valid_cancel_stream(*result, cancel, 2, 3, maximum_sequence) || !event ||
            event->price.ticks() != maximum_units || event->cancelled_quantity.units() != maximum_units ||
            event->order_id != cancel.target_order_id || event->participant_id != cancel.participant_id || !exchange.book().empty()) {
            return false;
        }
        auto backward = cancel;
        backward.timestamp = {};
        const auto refusal = exchange.process_cancel(backward);
        const auto duplicate = exchange.process_order(order);
        if (!refusal || !cancel_rejected(*refusal, backward, RejectionReason::InvalidTimestamp) ||
            !valid_cancel_stream(*refusal, backward, 3, 4, maximum_sequence) ||
            !duplicate || !rejected(*duplicate, order, RejectionReason::DuplicateOrderId)) {
            return false;
        }
    }
    return true;
}

int main() {
    if (!test_cancellation_and_sequences() || !test_cancel_validation_and_clock() ||
        !test_cancel_numeric_boundaries() || !test_configuration() || !test_matching_and_sequences() || !test_validation_precedence() ||
        !test_lifetime_ids_and_clock() || !test_numeric_boundaries()) {
        std::cerr << "exchange acceptance, validation, identity, or sequencing failed\n";
        return 1;
    }
}
