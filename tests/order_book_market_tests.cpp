#include <marketlab/order_book.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <variant>

using marketlab::AcceptedOrder;
using marketlab::EventHeader;
using marketlab::ExchangeEvent;
using marketlab::Execution;
using marketlab::InstrumentId;
using marketlab::MarketRemainderExpired;
using marketlab::MatchError;
using marketlab::OrderAccepted;
using marketlab::OrderBook;
using marketlab::OrderFilled;
using marketlab::OrderId;
using marketlab::OrderPartiallyFilled;
using marketlab::OrderResult;
using marketlab::OrderRested;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::PassiveInsertResult;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::Side;
using marketlab::is_valid;

AcceptedOrder make_order(std::uint64_t id, std::uint64_t sequence, Side side,
                         std::int64_t price, std::int64_t units) {
    return {OrderId{id}, ParticipantId{sequence}, InstrumentId{1}, side, OrderType::Limit,
        Quantity::from_units(units).value(), Price::from_ticks(price).value(), {0}, {sequence}};
}

AcceptedOrder make_market(std::uint64_t id, std::uint64_t sequence, Side side, std::int64_t units) {
    auto order = make_order(id, sequence, side, 1, units);
    order.type = OrderType::Market;
    order.limit_price.reset();
    return order;
}

bool valid_stream(const OrderResult& result, const AcceptedOrder& incoming, std::uint64_t first) {
    if (result.events.empty()) {
        return false;
    }
    const auto* accepted = std::get_if<OrderAccepted>(&result.events.front());
    if (!accepted || accepted->order.type != OrderType::Market || accepted->order.limit_price ||
        accepted->order.order_id != incoming.order_id || accepted->order.original_quantity != incoming.original_quantity) {
        return false;
    }
    for (std::size_t index = 0; index < result.events.size(); ++index) {
        if (!is_valid(result.events[index]) || std::holds_alternative<OrderRested>(result.events[index])) {
            return false;
        }
        const auto header = std::visit([](const auto& event) {
            if constexpr (std::is_same_v<std::decay_t<decltype(event)>, Execution>) {
                return EventHeader{event.timestamp, event.event_sequence, event.request_sequence, event.instrument_id};
            } else {
                return event.header;
            }
        }, result.events[index]);
        if (header.timestamp != incoming.accepted_at || header.request_sequence != incoming.arrival_sequence ||
            header.instrument_id != incoming.instrument_id || header.event_sequence.value != first + index) {
            return false;
        }
    }
    if (result.remaining_quantity.units() != 0) {
        const auto* expired = std::get_if<MarketRemainderExpired>(&result.events.back());
        return expired && expired->order_id == incoming.order_id && expired->participant_id == incoming.participant_id &&
            expired->remaining_quantity == result.remaining_quantity;
    }
    return std::holds_alternative<OrderFilled>(result.events.back());
}

bool valid_status(const ExchangeEvent& event, OrderId id, ParticipantId owner, Quantity remaining) {
    if (remaining.units() == 0) {
        const auto* filled = std::get_if<OrderFilled>(&event);
        return filled && filled->order_id == id && filled->participant_id == owner && filled->remaining_quantity == remaining;
    }
    const auto* partial = std::get_if<OrderPartiallyFilled>(&event);
    return partial && partial->order_id == id && partial->participant_id == owner && partial->remaining_quantity == remaining;
}

bool test_matching() {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        const auto worse_price = side == Side::Buy ? 1000 : 1;
        for (const auto units : std::array<std::int64_t, 8>{1, 3, 4, 8, 9, 15, 17, maximum}) {
            OrderBook book{InstrumentId{1}};
            const auto worse = make_order(20, 1, opposite, worse_price, 7);
            const auto first = make_order(30, 2, opposite, 100, 5);
            const auto second = make_order(1, 3, opposite, 100, 5);
            const auto same_side = make_order(40, 4, side, side == Side::Buy ? 99 : 101, 4);
            if (book.insert_passive({worse, worse.original_quantity}) != PassiveInsertResult::Inserted ||
                book.insert_passive({first, Quantity::from_units(3).value()}) != PassiveInsertResult::Inserted ||
                book.insert_passive({second, second.original_quantity}) != PassiveInsertResult::Inserted ||
                book.insert_passive({same_side, same_side.original_quantity}) != PassiveInsertResult::Inserted) {
                return false;
            }
            auto incoming = make_market(99, 5, side, units);
            incoming.participant_id = first.participant_id;
            const auto result = book.process_order(incoming, {50});
            const std::size_t matches = units <= 3 ? 1 : units <= 8 ? 2 : 3;
            const auto remainder = Quantity::from_units(units > 15 ? units - 15 : 0).value();
            if (!result || result->remaining_quantity != remainder ||
                result->events.size() != 1 + 3 * matches + (remainder.units() > 0 ? 1 : 0) ||
                !valid_stream(*result, incoming, 50) || book.contains(incoming.order_id) || book.find(incoming.order_id)) {
                return false;
            }
            const std::array resting{first, second, worse};
            const std::array<std::int64_t, 3> quantities{3, 5, 7};
            auto remaining = incoming.original_quantity;
            for (std::size_t index = 0; index < matches; ++index) {
                const auto available = Quantity::from_units(quantities[index]).value();
                const auto quantity = std::min(remaining, available);
                const auto rest_remaining = available.subtract(quantity).value();
                remaining = remaining.subtract(quantity).value();
                const auto* execution = std::get_if<Execution>(&result->events[1 + 3 * index]);
                if (!execution || execution->resting_order_id != resting[index].order_id ||
                    execution->aggressive_order_id != incoming.order_id || execution->aggressive_side != side ||
                    execution->price != *resting[index].limit_price || execution->quantity != quantity ||
                    execution->buyer_id != (side == Side::Buy ? incoming.participant_id : resting[index].participant_id) ||
                    execution->seller_id != (side == Side::Buy ? resting[index].participant_id : incoming.participant_id) ||
                    !valid_status(result->events[2 + 3 * index], resting[index].order_id,
                        resting[index].participant_id, rest_remaining) ||
                    !valid_status(result->events[3 + 3 * index], incoming.order_id, incoming.participant_id, remaining) ||
                    book.contains(resting[index].order_id) != (rest_remaining.units() > 0)) {
                    return false;
                }
            }
            const auto* head = side == Side::Buy ? book.best_ask() : book.best_bid();
            if (units < 15) {
                const auto expected = units < 3 ? first : units < 8 ? second : worse;
                const auto expected_units = units < 3 ? 3 - units : units < 8 ? 8 - units : 15 - units;
                if (!head || head->order.order_id != expected.order_id || head->remaining_quantity.units() != expected_units ||
                    head->order.original_quantity != expected.original_quantity || head->order.arrival_sequence != expected.arrival_sequence) {
                    return false;
                }
            } else if (head || book.order_count() != 1) {
                return false;
            }
            const auto* same = book.find(same_side.order_id);
            if (!same || same->remaining_quantity.units() != 4) {
                return false;
            }
            const auto stale = make_market(98, 5, side, 1);
            const auto invalid_sequence = book.process_order(stale, {100});
            if (invalid_sequence || invalid_sequence.error() != MatchError::OutOfOrder) {
                return false;
            }
        }
    }
    return true;
}

bool test_empty_and_capacity() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const bool same_side_only : {false, true}) {
            OrderBook book{InstrumentId{1}};
            const auto own = make_order(1, 1, side, 100, 5);
            if (same_side_only && book.insert_passive({own, own.original_quantity}) != PassiveInsertResult::Inserted) {
                return false;
            }
            const auto incoming = make_market(2, 2, side, 7);
            const auto* before = book.find(own.order_id);
            const auto failure = book.process_order(incoming, {maximum});
            if (failure || failure.error() != MatchError::EventSequenceExhausted || book.find(own.order_id) != before) {
                return false;
            }
            const auto result = book.process_order(incoming, {maximum - 1});
            if (!result || result->events.size() != 2 || result->remaining_quantity.units() != 7 ||
                !valid_stream(*result, incoming, maximum - 1) || book.find(incoming.order_id) ||
                book.order_count() != (same_side_only ? 1U : 0U)) {
                return false;
            }
        }
        for (const auto units : {1, 5, 7}) {
            OrderBook book{InstrumentId{1}};
            const auto resting = make_order(1, 1, side == Side::Buy ? Side::Sell : Side::Buy, 100, 5);
            if (book.insert_passive({resting, resting.original_quantity}) != PassiveInsertResult::Inserted) {
                return false;
            }
            const auto incoming = make_market(2, 2, side, units);
            const std::uint64_t count = units <= 5 ? 4 : 5;
            const auto failure = book.process_order(incoming, {maximum - count + 2});
            const auto* preserved = book.find(resting.order_id);
            if (failure || failure.error() != MatchError::EventSequenceExhausted || !preserved ||
                preserved->remaining_quantity.units() != 5 || book.find(incoming.order_id)) {
                return false;
            }
            const auto result = book.process_order(incoming, {maximum - count + 1});
            if (!result || result->events.size() != count || !valid_stream(*result, incoming, maximum - count + 1)) {
                return false;
            }
        }
    }
    return true;
}

bool test_invalid_and_boundaries() {
    OrderBook book{InstrumentId{1}};
    const auto resting = make_order(1, 1, Side::Sell, 100, 5);
    if (book.insert_passive({resting, resting.original_quantity}) != PassiveInsertResult::Inserted) {
        return false;
    }
    const auto candidate = make_market(2, 2, Side::Buy, 5);
    const auto expect_error = [&](const AcceptedOrder& order, marketlab::EventSequence sequence, MatchError expected) {
        const auto result = book.process_order(order, sequence);
        const auto* preserved = book.find(resting.order_id);
        return !result && result.error() == expected && book.order_count() == 1 && preserved &&
            preserved->remaining_quantity.units() == 5 && !book.best_bid();
    };
    for (int field = 0; field < 8; ++field) {
        auto invalid = candidate;
        switch (field) {
        case 0: invalid.order_id = {}; break;
        case 1: invalid.participant_id = {}; break;
        case 2: invalid.instrument_id = {}; break;
        case 3: invalid.side = static_cast<Side>(3); break;
        case 4: invalid.limit_price = Price::from_ticks(100).value(); break;
        case 5: invalid.arrival_sequence = {}; break;
        case 6: invalid.original_quantity = Quantity::from_units(0).value(); break;
        case 7: invalid.type = static_cast<OrderType>(3); break;
        }
        if (!expect_error(invalid, {1}, MatchError::InvalidOrder)) {
            return false;
        }
    }
    auto wrong = candidate;
    wrong.instrument_id = {2};
    auto duplicate = candidate;
    duplicate.order_id = resting.order_id;
    auto stale = candidate;
    stale.arrival_sequence = {1};
    if (!expect_error(wrong, {1}, MatchError::WrongInstrument) ||
        !expect_error(duplicate, {1}, MatchError::DuplicateOrderId) ||
        !expect_error(stale, {1}, MatchError::OutOfOrder) ||
        !expect_error(candidate, {}, MatchError::InvalidEventSequence) || !book.process_order(candidate, {1})) {
        return false;
    }
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
    for (const auto side : {Side::Buy, Side::Sell}) {
        OrderBook boundaries{InstrumentId{1}};
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        const auto first_price = side == Side::Buy ? 1 : maximum;
        const auto second_price = side == Side::Buy ? maximum : 1;
        const auto first = make_order(1, 1, opposite, first_price, maximum - 1);
        const auto second = make_order(2, 2, opposite, second_price, maximum);
        auto incoming = make_market(maximum_sequence, maximum_sequence, side, maximum);
        incoming.accepted_at = {maximum_sequence};
        if (boundaries.insert_passive({first, first.original_quantity}) != PassiveInsertResult::Inserted ||
            boundaries.insert_passive({second, second.original_quantity}) != PassiveInsertResult::Inserted) {
            return false;
        }
        const auto result = boundaries.process_order(incoming, {maximum_sequence - 6});
        const auto* remaining = boundaries.find(second.order_id);
        if (!result || result->events.size() != 7 || !valid_stream(*result, incoming, maximum_sequence - 6) ||
            result->remaining_quantity.units() != 0 || !remaining || remaining->remaining_quantity.units() != maximum - 1 ||
            std::get<Execution>(result->events[1]).quantity.units() != maximum - 1 ||
            std::get<Execution>(result->events[1]).price.ticks() != first_price ||
            std::get<Execution>(result->events[4]).quantity.units() != 1 ||
            std::get<Execution>(result->events[4]).price.ticks() != second_price || boundaries.find(incoming.order_id)) {
            return false;
        }
    }
    return true;
}

int main() {
    if (!test_matching() || !test_empty_and_capacity() || !test_invalid_and_boundaries()) {
        std::cerr << "market-order matching, expiry, or validation failed\n";
        return 1;
    }
}
