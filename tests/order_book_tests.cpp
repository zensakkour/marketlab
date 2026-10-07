#include <marketlab/order_book.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

using marketlab::AcceptedOrder;
using marketlab::InstrumentId;
using marketlab::OrderBook;
using marketlab::OrderId;
using marketlab::OrderType;
using marketlab::PassiveInsertResult;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RestingOrder;
using marketlab::Side;

static_assert(!std::is_copy_constructible_v<OrderBook>);
static_assert(!std::is_move_constructible_v<OrderBook>);
static_assert(std::is_same_v<decltype(std::declval<const OrderBook&>().best_bid()),
                             const RestingOrder*>);
static_assert(std::is_same_v<decltype(std::declval<const OrderBook&>().best_ask()),
                             const RestingOrder*>);

RestingOrder make_order(std::uint64_t id, std::uint64_t sequence, Side side,
                        std::int64_t ticks) {
    const auto quantity = Quantity::from_units(5).value();
    return {
        AcceptedOrder{
            .order_id = {id},
            .participant_id = {2},
            .instrument_id = {1},
            .side = side,
            .type = OrderType::Limit,
            .original_quantity = quantity,
            .limit_price = Price::from_ticks(ticks).value(),
            .accepted_at = {0},
            .arrival_sequence = {sequence},
        },
        quantity,
    };
}

int main() {
    try {
        const OrderBook invalid{InstrumentId{0}};
        std::cerr << "zero instrument configured an order book\n";
        return 1;
    } catch (const std::invalid_argument&) {
    }
    OrderBook book{InstrumentId{1}};
    if (book.instrument_id() != InstrumentId{1} || !book.empty() || book.order_count() != 0 ||
        book.best_bid() || book.best_ask() || book.contains(OrderId{1})) {
        std::cerr << "unexpected empty book state\n";
        return 1;
    }

    struct InsertionCase {
        RestingOrder order;
        OrderId best_bid_id;
        OrderId best_ask_id;
    };
    const std::array insertions{
        InsertionCase{make_order(30, 1, Side::Buy, 100), {30}, {0}},
        InsertionCase{make_order(20, 2, Side::Buy, 98), {30}, {0}},
        InsertionCase{make_order(99, 3, Side::Buy, 101), {99}, {0}},
        InsertionCase{make_order(1, 4, Side::Buy, 101), {99}, {0}},
        InsertionCase{make_order(70, 5, Side::Sell, 110), {99}, {70}},
        InsertionCase{make_order(80, 6, Side::Sell, 112), {99}, {70}},
        InsertionCase{make_order(60, 7, Side::Sell, 109), {99}, {60}},
        InsertionCase{make_order(2, 8, Side::Sell, 109), {99}, {60}},
        InsertionCase{make_order(40, 9, Side::Buy, 99), {99}, {60}},
        InsertionCase{make_order(90, 10, Side::Sell, 111), {99}, {60}},
    };
    for (std::size_t index = 0; index < insertions.size(); ++index) {
        const auto& test = insertions[index];
        if (book.insert_passive(test.order) != PassiveInsertResult::Inserted || book.empty() ||
            book.order_count() != index + 1 || !book.contains(test.order.order.order_id)) {
            std::cerr << "passive insertion did not update active state\n";
            return 1;
        }
        const auto* bid = book.best_bid();
        const auto* ask = book.best_ask();
        if (!bid || bid->order.order_id != test.best_bid_id ||
            (ask ? ask->order.order_id != test.best_ask_id : test.best_ask_id != OrderId{0})) {
            std::cerr << "best price or FIFO head is incorrect\n";
            return 1;
        }
    }

    const auto candidate = make_order(100, 11, Side::Buy, 100);
    const auto expect_failure = [&](RestingOrder attempt, PassiveInsertResult expected) {
        const bool already_active = book.contains(attempt.order.order_id);
        const auto* bid = book.best_bid();
        const auto* ask = book.best_ask();
        const auto result = book.insert_passive(attempt);
        return result == expected && book.order_count() == insertions.size() &&
               book.contains(attempt.order.order_id) == already_active &&
               book.best_bid() == bid && book.best_ask() == ask &&
               bid->order.order_id == OrderId{99} && ask->order.order_id == OrderId{60} &&
               bid->remaining_quantity.units() == 5 && ask->remaining_quantity.units() == 5;
    };
    for (int field = 0; field < 8; ++field) {
        auto invalid = candidate;
        switch (field) {
        case 0:
            invalid.order.order_id = {};
            break;
        case 1:
            invalid.order.participant_id = {};
            break;
        case 2:
            invalid.order.instrument_id = {};
            break;
        case 3:
            invalid.order.side = static_cast<Side>(0);
            break;
        case 4:
            invalid.order.limit_price.reset();
            break;
        case 5:
            invalid.order.arrival_sequence = {};
            break;
        case 6:
            invalid.remaining_quantity = Quantity::from_units(0).value();
            break;
        case 7:
            invalid.remaining_quantity = Quantity::from_units(6).value();
            break;
        }
        if (!expect_failure(invalid, PassiveInsertResult::InvalidOrder)) {
            std::cerr << "invalid resting record changed the book\n";
            return 1;
        }
    }
    auto market = candidate;
    market.order.type = OrderType::Market;
    market.order.limit_price.reset();
    auto wrong_instrument = candidate;
    wrong_instrument.order.instrument_id = {2};
    auto duplicate = candidate;
    duplicate.order.order_id = {99};
    if (!expect_failure(market, PassiveInsertResult::InvalidOrder) ||
        !expect_failure(wrong_instrument, PassiveInsertResult::WrongInstrument) ||
        !expect_failure(duplicate, PassiveInsertResult::DuplicateOrderId) ||
        !expect_failure(make_order(100, 10, Side::Buy, 100), PassiveInsertResult::OutOfOrder) ||
        !expect_failure(make_order(100, 2, Side::Buy, 100), PassiveInsertResult::OutOfOrder)) {
        std::cerr << "invalid identity, type, or arrival order changed the book\n";
        return 1;
    }
    for (const auto side_price : std::array{
             std::pair{Side::Buy, std::int64_t{109}},
             std::pair{Side::Buy, std::int64_t{110}},
             std::pair{Side::Sell, std::int64_t{101}},
             std::pair{Side::Sell, std::int64_t{100}},
         }) {
        if (!expect_failure(make_order(100, 11, side_price.first, side_price.second),
                            PassiveInsertResult::WouldCross)) {
            std::cerr << "crossing insertion changed the passive book\n";
            return 1;
        }
    }
    if (book.insert_passive(candidate) != PassiveInsertResult::Inserted ||
        book.order_count() != insertions.size() + 1 || !book.contains(OrderId{100})) {
        std::cerr << "failed insertion reserved an ID or advanced arrival priority\n";
        return 1;
    }

    constexpr auto maximum_units = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
    OrderBook boundaries{InstrumentId{1}};
    auto small_bid = make_order(1, 1, Side::Buy, 1);
    small_bid.order.original_quantity = Quantity::from_units(1).value();
    small_bid.remaining_quantity = small_bid.order.original_quantity;
    auto large_ask = make_order(2, maximum_sequence, Side::Sell, maximum_units);
    large_ask.order.original_quantity = Quantity::from_units(maximum_units).value();
    large_ask.remaining_quantity = large_ask.order.original_quantity;
    large_ask.order.accepted_at = {maximum_sequence};
    if (boundaries.insert_passive(small_bid) != PassiveInsertResult::Inserted ||
        boundaries.insert_passive(large_ask) != PassiveInsertResult::Inserted ||
        boundaries.best_bid()->order.limit_price->ticks() != 1 ||
        boundaries.best_bid()->remaining_quantity.units() != 1 ||
        boundaries.best_ask()->order.limit_price->ticks() != maximum_units ||
        boundaries.best_ask()->remaining_quantity.units() != maximum_units) {
        std::cerr << "passive book lost exact numeric boundaries\n";
        return 1;
    }
    OrderBook partial_book{InstrumentId{1}};
    auto partial = make_order(1, 1, Side::Sell, 101);
    partial.remaining_quantity = Quantity::from_units(3).value();
    if (partial_book.insert_passive(partial) != PassiveInsertResult::Inserted ||
        partial_book.best_bid() || partial_book.best_ask()->remaining_quantity.units() != 3 ||
        partial_book.best_ask()->order.original_quantity.units() != 5 ||
        partial_book.best_ask()->order.arrival_sequence.value != 1) {
        std::cerr << "resting storage lost an accepted remainder or its priority\n";
        return 1;
    }
}
