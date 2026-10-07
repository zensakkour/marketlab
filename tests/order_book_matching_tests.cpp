#include <marketlab/order_book.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>

using marketlab::AcceptedOrder;
using marketlab::EventSequence;
using marketlab::InstrumentId;
using marketlab::MatchError;
using marketlab::OrderBook;
using marketlab::OrderId;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::PassiveInsertResult;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RestingOrder;
using marketlab::Side;
using marketlab::is_valid;

static_assert(std::is_nothrow_copy_constructible_v<marketlab::SingleMatch>);

AcceptedOrder make_order(std::uint64_t id, std::uint64_t sequence, Side side,
                         std::int64_t ticks, std::int64_t units) {
    return {
        .order_id = {id},
        .participant_id = {2},
        .instrument_id = {1},
        .side = side,
        .type = OrderType::Limit,
        .original_quantity = Quantity::from_units(units).value(),
        .limit_price = Price::from_ticks(ticks).value(),
        .accepted_at = {sequence},
        .arrival_sequence = {sequence},
    };
}

int main() {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    struct FillCase {
        std::int64_t incoming;
        std::int64_t resting;
        std::int64_t executed;
        std::int64_t incoming_remaining;
        std::int64_t resting_remaining;
        bool self_match;
    };
    constexpr std::array fills{
        FillCase{2, 5, 2, 0, 3, false},
        FillCase{5, 5, 5, 0, 0, false},
        FillCase{7, 5, 5, 2, 0, false},
        FillCase{2, 5, 2, 0, 3, true},
        FillCase{maximum, maximum, maximum, 0, 0, false},
        FillCase{1, maximum, 1, 0, maximum - 1, false},
        FillCase{maximum, 1, 1, maximum - 1, 0, false},
    };
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto resting_side = side == Side::Buy ? Side::Sell : Side::Buy;
        for (const auto& test : fills) {
            OrderBook book{InstrumentId{1}};
            auto passive = make_order(1, 1, resting_side, 101, test.resting);
            passive.participant_id = {1};
            if (book.insert_passive({passive, passive.original_quantity}) !=
                PassiveInsertResult::Inserted) {
                std::cerr << "could not prepare a resting order\n";
                return 1;
            }
            auto incoming = make_order(99, 2, side, side == Side::Buy ? 102 : 100, test.incoming);
            if (test.self_match) {
                incoming.participant_id = passive.participant_id;
            }
            const auto result = book.match_one(incoming, incoming.original_quantity, EventSequence{50});
            if (!result || !*result) {
                std::cerr << "eligible limit did not match\n";
                return 1;
            }
            const auto& match = **result;
            const auto& execution = match.execution;
            const auto expected_buyer = side == Side::Buy ? incoming.participant_id : passive.participant_id;
            const auto expected_seller = side == Side::Buy ? passive.participant_id : incoming.participant_id;
            if (!is_valid(execution) || execution.price.ticks() != 101 ||
                execution.quantity.units() != test.executed ||
                execution.aggressive_order_id != incoming.order_id ||
                execution.resting_order_id != passive.order_id || execution.aggressive_side != side ||
                execution.buyer_id != expected_buyer || execution.seller_id != expected_seller ||
                execution.timestamp != incoming.accepted_at || execution.event_sequence != EventSequence{50} ||
                execution.request_sequence != incoming.arrival_sequence ||
                execution.instrument_id != InstrumentId{1} ||
                match.incoming_remaining.units() != test.incoming_remaining ||
                match.resting_remaining.units() != test.resting_remaining ||
                incoming.original_quantity.units() != test.incoming || book.contains(incoming.order_id)) {
                std::cerr << "execution payload or quantity conservation is incorrect\n";
                return 1;
            }
            const auto* remaining = side == Side::Buy ? book.best_ask() : book.best_bid();
            if (test.resting_remaining == 0) {
                if (!book.empty() || book.contains(passive.order_id) || remaining ||
                    book.best_bid() || book.best_ask()) {
                    std::cerr << "full fill left an order, index entry, or empty price level\n";
                    return 1;
                }
            } else if (!remaining || !book.contains(passive.order_id) || book.order_count() != 1 ||
                       remaining->remaining_quantity.units() != test.resting_remaining ||
                       remaining->order.original_quantity != passive.original_quantity ||
                       remaining->order.arrival_sequence != passive.arrival_sequence ||
                       remaining->order.accepted_at != passive.accepted_at) {
                std::cerr << "partial fill changed identity or original priority\n";
                return 1;
            }
        }

        for (const auto ticks : std::array<std::int64_t, 2>{1, maximum}) {
            OrderBook book{InstrumentId{1}};
            const auto passive = make_order(1, 1, resting_side, ticks, maximum);
            const auto incoming = make_order(2, 2, side, ticks, maximum);
            if (book.insert_passive({passive, passive.original_quantity}) != PassiveInsertResult::Inserted) {
                return 1;
            }
            constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
            const auto result = book.match_one(incoming, incoming.original_quantity,
                                               EventSequence{maximum_sequence});
            if (!result || !*result || (**result).execution.price.ticks() != ticks ||
                (**result).execution.quantity.units() != maximum ||
                (**result).execution.event_sequence.value != maximum_sequence || !book.empty()) {
                std::cerr << "equality crossing lost numeric boundaries\n";
                return 1;
            }
        }

        OrderBook no_match_book{InstrumentId{1}};
        auto incoming = make_order(99, 2, side, side == Side::Buy ? 100 : 102, 5);
        auto result = no_match_book.match_one(incoming, incoming.original_quantity, EventSequence{1});
        if (!result || *result || !no_match_book.empty()) {
            std::cerr << "empty book produced a match\n";
            return 1;
        }
        OrderBook same_side_book{InstrumentId{1}};
        const auto own_side = make_order(1, 1, side, incoming.limit_price->ticks(), 5);
        if (same_side_book.insert_passive({own_side, own_side.original_quantity}) !=
            PassiveInsertResult::Inserted) {
            return 1;
        }
        const auto same_side_result = same_side_book.match_one(incoming, incoming.original_quantity, {1});
        if (!same_side_result || *same_side_result || same_side_book.order_count() != 1 ||
            !same_side_book.contains(own_side.order_id)) {
            std::cerr << "incoming order matched its own side\n";
            return 1;
        }
        const auto passive = make_order(1, 1, resting_side, 101, 5);
        if (no_match_book.insert_passive({passive, passive.original_quantity}) != PassiveInsertResult::Inserted) {
            return 1;
        }
        const auto* before = side == Side::Buy ? no_match_book.best_ask() : no_match_book.best_bid();
        result = no_match_book.match_one(incoming, incoming.original_quantity, EventSequence{1});
        const auto* after = side == Side::Buy ? no_match_book.best_ask() : no_match_book.best_bid();
        if (!result || *result || after != before || after->remaining_quantity.units() != 5 ||
            no_match_book.order_count() != 1 || !no_match_book.contains(passive.order_id)) {
            std::cerr << "non-crossing limit changed the book\n";
            return 1;
        }
    }

    OrderBook guarded{InstrumentId{1}};
    const auto bid = make_order(1, 1, Side::Buy, 100, 5);
    const auto ask = make_order(2, 2, Side::Sell, 110, 5);
    if (guarded.insert_passive({bid, bid.original_quantity}) != PassiveInsertResult::Inserted ||
        guarded.insert_passive({ask, ask.original_quantity}) != PassiveInsertResult::Inserted) {
        return 1;
    }
    const auto candidate = make_order(99, 3, Side::Buy, 110, 5);
    const auto expect_error = [&](const AcceptedOrder& incoming, Quantity remaining,
                                  EventSequence sequence, MatchError expected) {
        const auto* previous_bid = guarded.best_bid();
        const auto* previous_ask = guarded.best_ask();
        const auto result = guarded.match_one(incoming, remaining, sequence);
        return !result && result.error() == expected && guarded.order_count() == 2 &&
               guarded.best_bid() == previous_bid && guarded.best_ask() == previous_ask &&
               guarded.contains(bid.order_id) && guarded.contains(ask.order_id) &&
               previous_bid->remaining_quantity.units() == 5 && previous_ask->remaining_quantity.units() == 5;
    };
    for (int field = 0; field < 7; ++field) {
        auto invalid = candidate;
        switch (field) {
        case 0:
            invalid.order_id = {};
            break;
        case 1:
            invalid.participant_id = {};
            break;
        case 2:
            invalid.instrument_id = {};
            break;
        case 3:
            invalid.side = static_cast<Side>(0);
            break;
        case 4:
            invalid.limit_price.reset();
            break;
        case 5:
            invalid.arrival_sequence = {};
            break;
        case 6:
            invalid.original_quantity = Quantity::from_units(0).value();
            break;
        }
        if (!expect_error(invalid, candidate.original_quantity, {1}, MatchError::InvalidOrder)) {
            std::cerr << "invalid accepted order changed the book\n";
            return 1;
        }
    }
    auto market = candidate;
    market.type = OrderType::Market;
    market.limit_price.reset();
    auto wrong_instrument = candidate;
    wrong_instrument.instrument_id = {2};
    auto duplicate = candidate;
    duplicate.order_id = bid.order_id;
    auto out_of_order = candidate;
    out_of_order.arrival_sequence = {2};
    if (!expect_error(market, candidate.original_quantity, {1}, MatchError::InvalidOrder) ||
        !expect_error(candidate, Quantity::from_units(0).value(), {1}, MatchError::InvalidRemainingQuantity) ||
        !expect_error(candidate, Quantity::from_units(6).value(), {1}, MatchError::InvalidRemainingQuantity) ||
        !expect_error(wrong_instrument, candidate.original_quantity, {1}, MatchError::WrongInstrument) ||
        !expect_error(duplicate, candidate.original_quantity, {1}, MatchError::DuplicateOrderId) ||
        !expect_error(out_of_order, candidate.original_quantity, {1}, MatchError::OutOfOrder) ||
        !expect_error(candidate, candidate.original_quantity, {0}, MatchError::InvalidEventSequence)) {
        std::cerr << "invalid match input changed the book\n";
        return 1;
    }

    OrderBook prior_remainder{InstrumentId{1}};
    const auto original_resting = make_order(1, 1, Side::Sell, 101, 5);
    const auto original_incoming = make_order(2, 2, Side::Buy, 102, 7);
    if (prior_remainder.insert_passive({original_resting, Quantity::from_units(3).value()}) !=
        PassiveInsertResult::Inserted) {
        return 1;
    }
    const auto remainder_match = prior_remainder.match_one(
        original_incoming, Quantity::from_units(2).value(), {1});
    if (!remainder_match || !*remainder_match || (**remainder_match).execution.quantity.units() != 2 ||
        (**remainder_match).incoming_remaining.units() != 0 ||
        (**remainder_match).resting_remaining.units() != 1 ||
        prior_remainder.best_ask()->order.original_quantity.units() != 5 ||
        original_incoming.original_quantity.units() != 7) {
        std::cerr << "matching used original quantity instead of the supplied remainder\n";
        return 1;
    }

    // Repeated primitive calls expose FIFO and level cleanup; no whole-request
    // matching loop or automatic event sequencing is implemented here.
    for (const auto resting_side : {Side::Buy, Side::Sell}) {
        OrderBook book{InstrumentId{1}};
        const auto side = resting_side == Side::Buy ? Side::Sell : Side::Buy;
        const auto worse_price = resting_side == Side::Buy ? 99 : 101;
        const auto first = make_order(30, 1, resting_side, 100, 5);
        const auto second = make_order(1, 2, resting_side, 100, 5);
        const auto worse = make_order(20, 3, resting_side, worse_price, 5);
        for (const auto& order : {first, second, worse}) {
            if (book.insert_passive({order, order.original_quantity}) != PassiveInsertResult::Inserted) {
                return 1;
            }
        }
        constexpr std::array<std::int64_t, 4> quantities{2, 3, 5, 5};
        constexpr std::array<std::uint64_t, 4> resting_ids{30, 30, 1, 20};
        for (std::size_t index = 0; index < quantities.size(); ++index) {
            const auto incoming = make_order(100 + index, 4 + index, side,
                                             side == Side::Buy ? 101 : 99, quantities[index]);
            const auto result = book.match_one(incoming, incoming.original_quantity,
                                               EventSequence{10 + index});
            if (!result || !*result || (**result).execution.resting_order_id.value != resting_ids[index] ||
                (**result).execution.price.ticks() != (index == 3 ? worse_price : 100) ||
                (**result).incoming_remaining.units() != 0 ||
                (**result).resting_remaining.units() != (index == 0 ? 3 : 0)) {
                std::cerr << "partial fill or dequeue lost price-time priority\n";
                return 1;
            }
            if (index > 0 && book.contains(OrderId{resting_ids[index]})) {
                std::cerr << "completed resting order remained active\n";
                return 1;
            }
        }
        if (!book.empty() || book.best_bid() || book.best_ask()) {
            std::cerr << "dequeued orders left a price level\n";
            return 1;
        }
    }
}
