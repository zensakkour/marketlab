#include <marketlab/order_book.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

using marketlab::AcceptedOrder;
using marketlab::CancelError;
using marketlab::CancelRequest;
using marketlab::InstrumentId;
using marketlab::OrderBook;
using marketlab::OrderType;
using marketlab::PassiveInsertResult;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RestingOrder;
using marketlab::Side;

AcceptedOrder order(std::uint64_t id, std::uint64_t sequence, Side side, std::int64_t price, std::int64_t units = 5) {
    return {{id}, {1}, {1}, side, OrderType::Limit, Quantity::from_units(units).value(),
        Price::from_ticks(price).value(), {0}, {sequence}};
}

bool prepare(OrderBook& book) {
    const std::array orders{order(1, 1, Side::Buy, 100), order(2, 2, Side::Buy, 100),
        order(3, 3, Side::Buy, 99), order(4, 4, Side::Sell, 101), order(5, 5, Side::Sell, 102)};
    for (const auto& accepted : orders) {
        const auto remaining = Quantity::from_units(accepted.order_id.value == 1 ? 3 : 5).value();
        if (book.insert_passive({accepted, remaining}) != PassiveInsertResult::Inserted || !book.check_invariants()) {
            return false;
        }
    }
    return true;
}

bool test_operation_boundaries() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        OrderBook book{InstrumentId{1}};
        if (!book.check_invariants() || !prepare(book)) {
            return false;
        }
        const auto* bid = book.best_bid();
        const auto* ask = book.best_ask();
        if (!book.check_invariants() || book.best_bid() != bid || book.best_ask() != ask || book.order_count() != 5) {
            return false;
        }
        const auto crossing = order(9, 6, side, side == Side::Buy ? 101 : 100);
        if (book.insert_passive({crossing, crossing.original_quantity}) != PassiveInsertResult::WouldCross ||
            !book.check_invariants() || book.best_bid() != bid || book.best_ask() != ask) {
            return false;
        }
        const auto incoming = order(10, 6, side, side == Side::Buy ? 101 : 100, 6);
        const auto count = book.match_count(incoming, incoming.original_quantity);
        if (!count || *count != (side == Side::Buy ? 1U : 2U) || !book.check_invariants()) {
            return false;
        }
        auto remaining = incoming.original_quantity;
        for (std::size_t index = 0; index < *count; ++index) {
            const auto match = book.match_one(incoming, remaining, {index + 1});
            if (!match || !*match || !book.check_invariants() ||
                ((*match)->resting_remaining.units() == 0 && book.find((*match)->execution.resting_order_id))) {
                return false;
            }
            remaining = (*match)->incoming_remaining;
        }
        if (remaining.units() > 0 &&
            (book.insert_passive({incoming, remaining}) != PassiveInsertResult::Inserted || !book.check_invariants())) {
            return false;
        }
        const CancelRequest wrong_owner{{0}, {1}, {2}, {2}};
        const auto rejected = book.cancel(wrong_owner, {7}, {9});
        if (rejected || rejected.error() != CancelError::NotOwner || !book.check_invariants()) {
            return false;
        }
        const CancelRequest cancel{{0}, {1}, {1}, {3}};
        if (!book.cancel(cancel, {7}, {9}) || book.find({3}) || !book.check_invariants()) {
            return false;
        }
        auto market = order(11, 8, side, 100, 10);
        market.type = OrderType::Market;
        market.limit_price.reset();
        if (!book.process_order(market, {10}) || book.find(market.order_id) || !book.check_invariants()) {
            return false;
        }
    }
    return true;
}

bool test_corrupted_records() {
    OrderBook book{InstrumentId{1}};
    if (!prepare(book)) {
        return false;
    }
    // Test corruption of mutable value records only; matching never runs until restoration.
    auto* first = const_cast<RestingOrder*>(book.find({1}));
    auto* second = const_cast<RestingOrder*>(book.find({2}));
    if (!first || !second) {
        return false;
    }
    const auto saved = *first;
    for (int field = 0; field < 14; ++field) {
        switch (field) {
        case 0: first->remaining_quantity = Quantity::from_units(0).value(); break;
        case 1: first->remaining_quantity = Quantity::from_units(6).value(); break;
        case 2: first->order.order_id = {}; break;
        case 3: first->order.participant_id = {}; break;
        case 4: first->order.instrument_id = {2}; break;
        case 5: first->order.side = Side::Sell; break;
        case 6: first->order.type = OrderType::Market; first->order.limit_price.reset(); break;
        case 7: first->order.limit_price.reset(); break;
        case 8: first->order.limit_price = Price::from_ticks(101).value(); break;
        case 9: first->order.arrival_sequence = {}; break;
        case 10: first->order.arrival_sequence = {6}; break;
        case 11: first->order.order_id = {99}; break;
        case 12: first->order.order_id = {2}; break;
        case 13: first->order.arrival_sequence = {3}; break;
        }
        const bool detected = !book.check_invariants();
        *first = saved;
        if (!detected || !book.check_invariants()) {
            std::cerr << "record corruption " << field << " was missed or restoration failed\n";
            return false;
        }
    }
    const auto saved_second = *second;
    first->order.arrival_sequence = {2};
    second->order.arrival_sequence = {1};
    const bool detected_fifo = !book.check_invariants();
    *first = saved;
    *second = saved_second;
    return detected_fifo && book.check_invariants();
}

bool test_numeric_boundaries() {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximum_id = std::numeric_limits<std::uint64_t>::max();
    for (const auto side : {Side::Buy, Side::Sell}) {
        OrderBook book{InstrumentId{maximum_id}};
        auto accepted = order(maximum_id, maximum_id, side, maximum, maximum);
        accepted.instrument_id = {maximum_id};
        accepted.participant_id = {maximum_id};
        accepted.accepted_at = {maximum_id};
        if (book.insert_passive({accepted, accepted.original_quantity}) != PassiveInsertResult::Inserted ||
            !book.check_invariants()) {
            return false;
        }
        const CancelRequest cancel{{maximum_id}, {maximum_id}, {maximum_id}, {maximum_id}};
        const auto failure = book.cancel(cancel, {maximum_id}, {maximum_id});
        if (failure || failure.error() != CancelError::OutOfOrder || !book.check_invariants()) {
            return false;
        }
    }
    return true;
}

int main() {
    if (!test_operation_boundaries() || !test_corrupted_records() || !test_numeric_boundaries()) {
        std::cerr << "book structural invariant validation failed\n";
        return 1;
    }
}
