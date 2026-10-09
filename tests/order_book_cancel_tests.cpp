#include <marketlab/order_book.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

using marketlab::AcceptedOrder;
using marketlab::CancelError;
using marketlab::CancelRequest;
using marketlab::EventSequence;
using marketlab::InstrumentId;
using marketlab::OrderBook;
using marketlab::OrderId;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::PassiveInsertResult;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RequestSequence;
using marketlab::RestingOrder;
using marketlab::Side;
using marketlab::is_valid;

RestingOrder make_order(std::uint64_t id, std::uint64_t sequence, Side side, std::int64_t ticks) {
    const auto quantity = Quantity::from_units(5).value();
    return {AcceptedOrder{OrderId{id}, ParticipantId{sequence}, InstrumentId{1}, side,
        OrderType::Limit, quantity, Price::from_ticks(ticks).value(), {0}, {sequence}}, quantity};
}

bool test_queue_removal() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        const std::array resting{
            make_order(30, 1, side, 100),
            make_order(1, 2, side, 100),
            make_order(20, 3, side, 100),
            make_order(10, 4, side, side == Side::Buy ? 99 : 101),
        };
        for (std::size_t target = 0; target < resting.size(); ++target) {
            OrderBook book{InstrumentId{1}};
            for (const auto& order : resting) {
                if (book.insert_passive(order) != PassiveInsertResult::Inserted) {
                    return false;
                }
            }
            const auto opposite = make_order(40, 5, side == Side::Buy ? Side::Sell : Side::Buy,
                side == Side::Buy ? 110 : 90);
            if (book.insert_passive(opposite) != PassiveInsertResult::Inserted) {
                return false;
            }
            const auto& removed = resting[target];
            const CancelRequest request{{20}, {1}, removed.order.participant_id, removed.order.order_id};
            const auto result = book.cancel(request, {6}, {50});
            if (!result || !is_valid(*result) || result->header.timestamp != request.timestamp ||
                result->header.instrument_id != request.instrument_id ||
                result->header.request_sequence != RequestSequence{6} ||
                result->header.event_sequence != EventSequence{50} || result->order_id != request.target_order_id ||
                result->participant_id != request.participant_id || result->price != *removed.order.limit_price ||
                result->cancelled_quantity.units() != 5 || book.contains(removed.order.order_id) ||
                book.find(removed.order.order_id) || book.order_count() != 4) {
                std::cerr << "cancellation returned incorrect data or retained active state\n";
                return false;
            }
            for (const auto& order : resting) {
                if (order.order.order_id == removed.order.order_id) {
                    continue;
                }
                const auto* found = book.find(order.order.order_id);
                if (!found || found->remaining_quantity != order.remaining_quantity ||
                    found->order.participant_id != order.order.participant_id ||
                    found->order.original_quantity != order.order.original_quantity ||
                    found->order.limit_price != order.order.limit_price ||
                    found->order.accepted_at != order.order.accepted_at ||
                    found->order.arrival_sequence != order.order.arrival_sequence) {
                    std::cerr << "cancellation changed a surviving order\n";
                    return false;
                }
            }
            auto stale = make_order(88, 6, opposite.order.side, opposite.order.limit_price->ticks());
            stale.order.accepted_at = request.timestamp;
            if (book.insert_passive(stale) != PassiveInsertResult::OutOfOrder) {
                std::cerr << "successful cancellation did not advance arrival priority\n";
                return false;
            }
            auto incoming = make_order(99, 7, opposite.order.side, side == Side::Buy ? 99 : 101).order;
            incoming.original_quantity = Quantity::from_units(20).value();
            incoming.accepted_at = request.timestamp;
            auto remaining = incoming.original_quantity;
            std::uint64_t execution_sequence = 100;
            for (const auto& order : resting) {
                if (order.order.order_id == removed.order.order_id) {
                    continue;
                }
                const auto match = book.match_one(incoming, remaining, {execution_sequence++});
                if (!match || !*match || (**match).execution.resting_order_id != order.order.order_id ||
                    (**match).execution.price != *order.order.limit_price) {
                    std::cerr << "cancellation changed survivor FIFO or best-price priority\n";
                    return false;
                }
                remaining = (**match).incoming_remaining;
            }
            if (book.order_count() != 1 || book.find(opposite.order.order_id) == nullptr ||
                (side == Side::Buy ? book.best_bid() : book.best_ask())) {
                std::cerr << "cancelled or matched levels were not removed\n";
                return false;
            }
        }
    }
    return true;
}

bool test_errors_and_partial_fill() {
    OrderBook book{InstrumentId{1}};
    const auto resting = make_order(1, 1, Side::Sell, 100);
    if (book.insert_passive(resting) != PassiveInsertResult::Inserted) {
        return false;
    }
    const CancelRequest request{{0}, {1}, resting.order.participant_id, resting.order.order_id};
    const auto expect_error = [&](const CancelRequest& attempt, RequestSequence sequence,
                                 EventSequence event, CancelError expected) {
        const auto* before = book.find(resting.order.order_id);
        const auto result = book.cancel(attempt, sequence, event);
        return !result && result.error() == expected && book.order_count() == 1 &&
            book.find(resting.order.order_id) == before && before->remaining_quantity.units() == 5 &&
            book.best_ask() == before && !book.best_bid();
    };
    auto invalid = request;
    invalid.instrument_id = {};
    invalid.participant_id = {};
    invalid.target_order_id = {};
    if (!expect_error(invalid, {2}, {1}, CancelError::InvalidInstrument)) {
        return false;
    }
    invalid.instrument_id = {2};
    if (!expect_error(invalid, {2}, {1}, CancelError::InvalidInstrument)) {
        return false;
    }
    invalid.instrument_id = {1};
    if (!expect_error(invalid, {2}, {1}, CancelError::InvalidParticipant)) {
        return false;
    }
    invalid.participant_id = request.participant_id;
    if (!expect_error(invalid, {2}, {1}, CancelError::InvalidOrderId) ||
        !expect_error(request, {}, {1}, CancelError::InvalidRequestSequence) ||
        !expect_error(request, {1}, {1}, CancelError::OutOfOrder) ||
        !expect_error(request, {2}, {}, CancelError::InvalidEventSequence)) {
        return false;
    }
    auto absent = request;
    absent.target_order_id = {99};
    absent.participant_id = {9};
    auto other_owner = request;
    other_owner.participant_id = {9};
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (!expect_error(absent, {maximum}, {1}, CancelError::NotActive) ||
        !expect_error(other_owner, {maximum}, {1}, CancelError::NotOwner)) {
        return false;
    }
    auto incoming = make_order(2, 2, Side::Buy, 100).order;
    incoming.original_quantity = Quantity::from_units(2).value();
    const auto match = book.match_one(incoming, incoming.original_quantity, {40});
    const auto cancelled = book.cancel(request, {3}, {41});
    if (!match || !*match || !cancelled || !is_valid(*cancelled) || cancelled->cancelled_quantity.units() != 3 ||
        (**match).execution.quantity.units() != 2 || (**match).execution.price.ticks() != 100 ||
        (**match).execution.event_sequence != EventSequence{40} || !book.empty() || book.find(request.target_order_id)) {
        std::cerr << "cancellation did not remove exactly the unfilled remainder\n";
        return false;
    }
    const auto repeated = book.cancel(request, {maximum}, {42});
    const auto unknown = book.cancel(absent, {maximum}, {42});
    if (repeated || repeated.error() != CancelError::NotActive || unknown || unknown.error() != CancelError::NotActive) {
        return false;
    }
    const auto next = make_order(3, 4, Side::Sell, 100);
    if (book.insert_passive(next) != PassiveInsertResult::Inserted) {
        std::cerr << "failed cancellation advanced book priority\n";
        return false;
    }
    incoming = make_order(4, 5, Side::Buy, 100).order;
    const auto fill = book.process_limit(incoming, {43});
    const CancelRequest filled_request{{0}, {1}, next.order.participant_id, next.order.order_id};
    const auto inactive = book.cancel(filled_request, {6}, {47});
    return fill && !inactive && inactive.error() == CancelError::NotActive && book.empty();
}

bool test_boundaries() {
    constexpr auto maximum_id = std::numeric_limits<std::uint64_t>::max();
    constexpr auto maximum_units = std::numeric_limits<std::int64_t>::max();
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const auto ticks : std::array<std::int64_t, 2>{1, maximum_units}) {
            OrderBook book{InstrumentId{1}};
            auto resting = make_order(maximum_id, 1, side, ticks);
            resting.order.participant_id = {maximum_id};
            resting.order.original_quantity = Quantity::from_units(maximum_units).value();
            resting.remaining_quantity = resting.order.original_quantity;
            if (book.insert_passive(resting) != PassiveInsertResult::Inserted) {
                return false;
            }
            const CancelRequest request{{maximum_id}, {1}, {maximum_id}, {maximum_id}};
            const auto result = book.cancel(request, {maximum_id}, {maximum_id});
            if (!result || !is_valid(*result) || result->cancelled_quantity.units() != maximum_units ||
                result->price.ticks() != ticks || result->order_id.value != maximum_id ||
                result->participant_id.value != maximum_id || result->header.timestamp.nanoseconds != maximum_id ||
                result->header.request_sequence.value != maximum_id || result->header.event_sequence.value != maximum_id ||
                !book.empty() || book.best_bid() || book.best_ask()) {
                std::cerr << "cancellation lost numeric boundaries or empty-level cleanup\n";
                return false;
            }
        }
    }
    return true;
}

int main() {
    if (!test_queue_removal() || !test_errors_and_partial_fill() || !test_boundaries()) {
        std::cerr << "cancellation tests failed\n";
        return 1;
    }
}
