#include <marketlab/order_book.hpp>

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
using marketlab::EventSequence;
using marketlab::InstrumentId;
using marketlab::LimitResult;
using marketlab::LogicalTime;
using marketlab::MatchError;
using marketlab::OrderAccepted;
using marketlab::OrderBook;
using marketlab::OrderFilled;
using marketlab::OrderId;
using marketlab::OrderPartiallyFilled;
using marketlab::OrderRested;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::PassiveInsertResult;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RequestSequence;
using marketlab::Side;
using marketlab::is_valid;

AcceptedOrder make_order(std::uint64_t id, std::uint64_t sequence, Side side,
                         std::int64_t price, std::int64_t quantity) {
    return {OrderId{id}, ParticipantId{sequence}, InstrumentId{1}, side, OrderType::Limit,
        Quantity::from_units(quantity).value(), Price::from_ticks(price).value(),
        LogicalTime{0}, RequestSequence{sequence}};
}

bool valid_stream(const LimitResult& result, const AcceptedOrder& incoming, std::uint64_t first) {
    if (result.events.empty()) {
        return false;
    }
    const auto* accepted = std::get_if<OrderAccepted>(&result.events.front());
    if (!accepted || accepted->order.order_id != incoming.order_id ||
        accepted->order.original_quantity != incoming.original_quantity ||
        accepted->order.limit_price != incoming.limit_price || accepted->order.side != incoming.side) {
        return false;
    }
    for (std::size_t index = 0; index < result.events.size(); ++index) {
        if (!is_valid(result.events[index])) {
            return false;
        }
        const auto header = std::visit([](const auto& event) {
            if constexpr (std::is_same_v<std::decay_t<decltype(event)>, Execution>) {
                return EventHeader{event.timestamp, event.event_sequence,
                    event.request_sequence, event.instrument_id};
            } else {
                return event.header;
            }
        }, result.events[index]);
        if (header.timestamp != incoming.accepted_at ||
            header.request_sequence != incoming.arrival_sequence ||
            header.instrument_id != incoming.instrument_id || header.event_sequence.value != first + index) {
            return false;
        }
    }
    return true;
}

bool valid_status(const ExchangeEvent& event, OrderId id, ParticipantId owner, Quantity remaining) {
    if (remaining.units() == 0) {
        const auto* filled = std::get_if<OrderFilled>(&event);
        return filled && filled->order_id == id && filled->participant_id == owner &&
            filled->remaining_quantity == remaining;
    }
    const auto* partial = std::get_if<OrderPartiallyFilled>(&event);
    return partial && partial->order_id == id && partial->participant_id == owner &&
        partial->remaining_quantity == remaining;
}

bool test_level_walk() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        const int direction = side == Side::Buy ? 1 : -1;
        for (const auto units : {1, 3, 4, 8, 9, 15, 17}) {
            OrderBook book{InstrumentId{1}};
            const auto worse = make_order(20, 1, opposite, 100 + direction, 7);
            const auto first = make_order(30, 2, opposite, 100, 5);
            const auto second = make_order(1, 3, opposite, 100, 5);
            const auto excluded = make_order(10, 4, opposite, 100 + 2 * direction, 9);
            const auto own_side = make_order(40, 5, side, side == Side::Buy ? 90 : 110, 4);
            if (book.insert_passive({worse, worse.original_quantity}) != PassiveInsertResult::Inserted ||
                book.insert_passive({first, Quantity::from_units(3).value()}) != PassiveInsertResult::Inserted ||
                book.insert_passive({second, second.original_quantity}) != PassiveInsertResult::Inserted ||
                book.insert_passive({excluded, excluded.original_quantity}) != PassiveInsertResult::Inserted ||
                book.insert_passive({own_side, own_side.original_quantity}) != PassiveInsertResult::Inserted) {
                return false;
            }
            auto incoming = make_order(99, 6, side, 100 + direction, units);
            incoming.participant_id = first.participant_id; // Include permitted self-matching.
            const auto result = book.process_limit(incoming, EventSequence{100});
            const std::size_t matches = units <= 3 ? 1 : units <= 8 ? 2 : 3;
            const auto final_remaining = Quantity::from_units(units > 15 ? units - 15 : 0).value();
            if (!result || result->remaining_quantity != final_remaining ||
                result->events.size() != 1 + 3 * matches + (final_remaining.units() > 0 ? 1 : 0) ||
                !valid_stream(*result, incoming, 100)) {
                std::cerr << "limit processing returned an invalid event stream\n";
                return false;
            }
            const std::array resting{first, second, worse};
            const std::array<std::int64_t, 3> resting_quantities{3, 5, 7};
            auto remaining = incoming.original_quantity;
            for (std::size_t index = 0; index < matches; ++index) {
                const auto quantity = std::min(remaining, Quantity::from_units(resting_quantities[index]).value());
                const auto rest_remaining = Quantity::from_units(resting_quantities[index]).value().subtract(quantity).value();
                remaining = remaining.subtract(quantity).value();
                const auto* execution = std::get_if<Execution>(&result->events[1 + 3 * index]);
                if (!execution || execution->quantity != quantity || execution->price != *resting[index].limit_price ||
                    execution->resting_order_id != resting[index].order_id ||
                    execution->aggressive_order_id != incoming.order_id || execution->aggressive_side != side ||
                    execution->buyer_id != (side == Side::Buy ? incoming.participant_id : resting[index].participant_id) ||
                    execution->seller_id != (side == Side::Buy ? resting[index].participant_id : incoming.participant_id) ||
                    !valid_status(result->events[2 + 3 * index], resting[index].order_id,
                        resting[index].participant_id, rest_remaining) ||
                    !valid_status(result->events[3 + 3 * index], incoming.order_id,
                        incoming.participant_id, remaining) ||
                    book.contains(resting[index].order_id) != (rest_remaining.units() != 0)) {
                    std::cerr << "limit processing lost FIFO, prices, quantities, or status ordering\n";
                    return false;
                }
            }
            const auto* head = side == Side::Buy ? book.best_ask() : book.best_bid();
            const auto expected_head = units < 3 ? first : units < 8 ? second : units < 15 ? worse : excluded;
            const auto expected_units = units < 3 ? 3 - units : units < 8 ? 8 - units : units < 15 ? 15 - units : 9;
            const auto* own_head = side == Side::Buy ? book.best_bid() : book.best_ask();
            if (!head || head->order.order_id != expected_head.order_id ||
                head->remaining_quantity.units() != expected_units ||
                head->order.original_quantity != expected_head.original_quantity ||
                head->order.arrival_sequence != expected_head.arrival_sequence ||
                !book.contains(excluded.order_id) || !book.contains(own_side.order_id) ||
                book.contains(incoming.order_id) != (final_remaining.units() > 0) ||
                !own_head || book.best_bid()->order.limit_price >= book.best_ask()->order.limit_price) {
                std::cerr << "limit processing left an incorrect resting book\n";
                return false;
            }
            if (final_remaining.units() > 0) {
                const auto* rested = std::get_if<OrderRested>(&result->events.back());
                if (!rested || rested->order_id != incoming.order_id || rested->participant_id != incoming.participant_id ||
                    rested->price != *incoming.limit_price || rested->remaining_quantity != final_remaining ||
                    own_head->order.order_id != incoming.order_id || own_head->remaining_quantity != final_remaining ||
                    own_head->order.original_quantity != incoming.original_quantity ||
                    own_head->order.arrival_sequence != incoming.arrival_sequence) {
                    return false;
                }
            } else if (own_head->order.order_id != own_side.order_id) {
                return false;
            }
            auto stale = incoming;
            stale.order_id = {98};
            const auto stale_result = book.process_limit(stale, {200});
            if (stale_result || stale_result.error() != MatchError::OutOfOrder) {
                std::cerr << "successful processing did not advance arrival priority\n";
                return false;
            }
        }
    }
    return true;
}

bool test_capacity_and_validation() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const auto units : {1, 5, 7}) {
            OrderBook book{InstrumentId{1}};
            const auto resting = make_order(1, 1, side == Side::Buy ? Side::Sell : Side::Buy, 100, 5);
            const auto incoming = make_order(2, 2, side, 100, units);
            if (book.insert_passive({resting, resting.original_quantity}) != PassiveInsertResult::Inserted) {
                return false;
            }
            const auto* before = side == Side::Buy ? book.best_ask() : book.best_bid();
            const std::uint64_t events = units <= 5 ? 4 : 5;
            const auto failure = book.process_limit(incoming, {maximum - events + 2});
            if (failure || failure.error() != MatchError::EventSequenceExhausted || book.order_count() != 1 ||
                (side == Side::Buy ? book.best_ask() : book.best_bid()) != before ||
                before->remaining_quantity.units() != 5 || !book.contains(resting.order_id) || book.contains(incoming.order_id)) {
                std::cerr << "event-counter exhaustion changed the book\n";
                return false;
            }
            const auto result = book.process_limit(incoming, {maximum - events + 1});
            if (!result || result->events.size() != events || !valid_stream(*result, incoming, maximum - events + 1)) {
                std::cerr << "exact event capacity was not usable\n";
                return false;
            }
        }
        OrderBook empty{InstrumentId{1}};
        const auto incoming = make_order(2, maximum, side, std::numeric_limits<std::int64_t>::max(),
            std::numeric_limits<std::int64_t>::max());
        const auto failure = empty.process_limit(incoming, {maximum});
        if (failure || failure.error() != MatchError::EventSequenceExhausted || !empty.empty()) {
            return false;
        }
        const auto result = empty.process_limit(incoming, {maximum - 1});
        if (!result || result->events.size() != 2 || result->remaining_quantity != incoming.original_quantity ||
            !valid_stream(*result, incoming, maximum - 1) || !empty.contains(incoming.order_id)) {
            std::cerr << "non-crossing limit did not rest at numeric boundaries\n";
            return false;
        }
    }
    OrderBook guarded{InstrumentId{1}};
    const auto passive = make_order(1, 1, Side::Sell, 100, 5);
    if (guarded.insert_passive({passive, passive.original_quantity}) != PassiveInsertResult::Inserted) {
        return false;
    }
    const auto candidate = make_order(2, 2, Side::Buy, 100, 5);
    const auto expect_error = [&](const AcceptedOrder& incoming, EventSequence sequence, MatchError error) {
        const auto result = guarded.process_limit(incoming, sequence);
        return !result && result.error() == error && guarded.order_count() == 1 &&
            guarded.best_ask()->remaining_quantity.units() == 5 && !guarded.best_bid();
    };
    for (int field = 0; field < 9; ++field) {
        auto invalid = candidate;
        switch (field) {
        case 0: invalid.order_id = {}; break;
        case 1: invalid.participant_id = {}; break;
        case 2: invalid.instrument_id = {}; break;
        case 3: invalid.side = static_cast<Side>(3); break;
        case 4: invalid.limit_price.reset(); break;
        case 5: invalid.arrival_sequence = {}; break;
        case 6: invalid.original_quantity = Quantity::from_units(0).value(); break;
        case 7: invalid.type = static_cast<OrderType>(3); break;
        case 8: invalid.type = OrderType::Market; invalid.limit_price.reset(); break;
        }
        if (!expect_error(invalid, {1}, MatchError::InvalidOrder)) {
            return false;
        }
    }
    auto wrong_instrument = candidate;
    wrong_instrument.instrument_id = {2};
    auto duplicate = candidate;
    duplicate.order_id = passive.order_id;
    auto stale = candidate;
    stale.arrival_sequence = {1};
    if (!expect_error(wrong_instrument, {1}, MatchError::WrongInstrument) ||
        !expect_error(duplicate, {1}, MatchError::DuplicateOrderId) ||
        !expect_error(stale, {1}, MatchError::OutOfOrder) ||
        !expect_error(candidate, {}, MatchError::InvalidEventSequence)) {
        return false;
    }
    return guarded.process_limit(candidate, {1}).has_value();
}

bool test_quantity_boundaries() {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const auto price : std::array<std::int64_t, 2>{1, maximum}) {
            OrderBook book{InstrumentId{1}};
            const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
            const auto first = make_order(1, 1, opposite, price, maximum - 1);
            const auto second = make_order(2, 2, opposite, price, maximum);
            const auto incoming = make_order(3, 3, side, price, maximum);
            if (book.insert_passive({first, first.original_quantity}) != PassiveInsertResult::Inserted ||
                book.insert_passive({second, second.original_quantity}) != PassiveInsertResult::Inserted) {
                return false;
            }
            const auto result = book.process_limit(incoming, {1});
            const auto* head = side == Side::Buy ? book.best_ask() : book.best_bid();
            if (!result || result->events.size() != 7 || result->remaining_quantity.units() != 0 ||
                !valid_stream(*result, incoming, 1) ||
                std::get<Execution>(result->events[1]).quantity.units() != maximum - 1 ||
                std::get<Execution>(result->events[4]).quantity.units() != 1 ||
                !head || head->order.order_id != second.order_id || head->remaining_quantity.units() != maximum - 1 ||
                book.contains(first.order_id) || book.contains(incoming.order_id)) {
                std::cerr << "limit processing lost exact numeric boundaries\n";
                return false;
            }
        }
    }
    return true;
}

int main() {
    if (!test_level_walk() || !test_capacity_and_validation() || !test_quantity_boundaries()) {
        std::cerr << "whole-limit tests failed\n";
        return 1;
    }
}
