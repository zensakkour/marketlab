#include <marketlab/exchange.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using marketlab::AcceptedOrder;
using marketlab::CancelRejected;
using marketlab::CancelRequest;
using marketlab::EventHeader;
using marketlab::Exchange;
using marketlab::ExchangeEvent;
using marketlab::Execution;
using marketlab::Instrument;
using marketlab::MarketRemainderExpired;
using marketlab::OrderAccepted;
using marketlab::OrderBook;
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
using marketlab::RestingOrder;
using marketlab::Side;
using marketlab::TickSize;

using Command = std::variant<OrderRequest, CancelRequest>;

OrderRequest limit(std::uint64_t id, Side side, std::int64_t price, std::int64_t units, std::uint64_t owner = 1) {
    return {{0}, {1}, {owner}, {id}, side, OrderType::Limit, units, price};
}

OrderRequest market(std::uint64_t id, Side side, std::int64_t units) {
    return {{0}, {1}, {2}, {id}, side, OrderType::Market, units, std::nullopt};
}

CancelRequest cancel(std::uint64_t id, std::uint64_t owner = 1) {
    return {{0}, {1}, {owner}, {id}};
}

Quantity quantity(std::int64_t units) {
    return Quantity::from_units(units).value();
}

EventHeader header(std::uint64_t event, std::uint64_t request) {
    return {{0}, {event}, {request}, {1}};
}

AcceptedOrder accepted_order(const OrderRequest& request, std::uint64_t sequence) {
    return {request.order_id, request.participant_id, request.instrument_id, request.side, request.type,
        quantity(request.quantity_units), request.limit_price_ticks ? Price::from_ticks(*request.limit_price_ticks) : std::nullopt,
        request.timestamp, {sequence}};
}

OrderAccepted accepted(const OrderRequest& request, EventHeader metadata) {
    return {metadata, accepted_order(request, metadata.request_sequence.value)};
}

OrderRested rested(const OrderRequest& request, std::int64_t remaining, EventHeader metadata) {
    return {metadata, request.order_id, request.participant_id,
        Price::from_ticks(*request.limit_price_ticks).value(), quantity(remaining)};
}

Execution trade(const OrderRequest& incoming, const OrderRequest& resting,
    std::int64_t price, std::int64_t units, EventHeader metadata) {
    const bool incoming_buy = incoming.side == Side::Buy;
    return {metadata.timestamp, metadata.event_sequence, metadata.request_sequence, metadata.instrument_id,
        incoming.order_id, resting.order_id, incoming.side,
        incoming_buy ? incoming.participant_id : resting.participant_id,
        incoming_buy ? resting.participant_id : incoming.participant_id,
        Price::from_ticks(price).value(), quantity(units)};
}

OrderFilled filled(const OrderRequest& order, EventHeader metadata) {
    return {metadata, order.order_id, order.participant_id, quantity(0)};
}

OrderPartiallyFilled partial(const OrderRequest& order, std::int64_t remaining, EventHeader metadata) {
    return {metadata, order.order_id, order.participant_id, quantity(remaining)};
}

MarketRemainderExpired expired(const OrderRequest& order, std::int64_t remaining, EventHeader metadata) {
    return {metadata, order.order_id, order.participant_id, quantity(remaining)};
}

OrderCancelled cancelled(const OrderRequest& order, std::int64_t remaining, EventHeader metadata) {
    return {metadata, order.order_id, order.participant_id,
        Price::from_ticks(*order.limit_price_ticks).value(), quantity(remaining)};
}

RestingOrder resting(const OrderRequest& request, std::uint64_t sequence, std::int64_t remaining) {
    return {accepted_order(request, sequence), quantity(remaining)};
}

bool same_request(const OrderRequest& left, const OrderRequest& right) {
    return left.timestamp == right.timestamp && left.instrument_id == right.instrument_id &&
        left.participant_id == right.participant_id && left.order_id == right.order_id && left.side == right.side &&
        left.type == right.type && left.quantity_units == right.quantity_units && left.limit_price_ticks == right.limit_price_ticks;
}

bool same_request(const CancelRequest& left, const CancelRequest& right) {
    return left.timestamp == right.timestamp && left.instrument_id == right.instrument_id &&
        left.participant_id == right.participant_id && left.target_order_id == right.target_order_id;
}

bool same_order(const AcceptedOrder& left, const AcceptedOrder& right) {
    return left.order_id == right.order_id && left.participant_id == right.participant_id &&
        left.instrument_id == right.instrument_id && left.side == right.side && left.type == right.type &&
        left.original_quantity == right.original_quantity && left.limit_price == right.limit_price &&
        left.accepted_at == right.accepted_at && left.arrival_sequence == right.arrival_sequence;
}

EventHeader event_header(const ExchangeEvent& event) {
    return std::visit([](const auto& record) {
        if constexpr (std::is_same_v<std::decay_t<decltype(record)>, Execution>) {
            return EventHeader{record.timestamp, record.event_sequence, record.request_sequence, record.instrument_id};
        } else {
            return record.header;
        }
    }, event);
}

bool same_event(const ExchangeEvent& actual, const ExchangeEvent& expected) {
    if (!marketlab::is_valid(actual) || !marketlab::is_valid(expected)) {
        return false;
    }
    const auto left = event_header(actual);
    const auto right = event_header(expected);
    if (left.timestamp != right.timestamp || left.event_sequence != right.event_sequence ||
        left.request_sequence != right.request_sequence || left.instrument_id != right.instrument_id) {
        return false;
    }
    return std::visit([&](const auto& record) {
        using Record = std::decay_t<decltype(record)>;
        const auto* wanted = std::get_if<Record>(&expected);
        if (!wanted) {
            return false;
        }
        if constexpr (std::is_same_v<Record, OrderAccepted>) {
            return same_order(record.order, wanted->order);
        } else if constexpr (std::is_same_v<Record, Execution>) {
            return record.aggressive_order_id == wanted->aggressive_order_id &&
                record.resting_order_id == wanted->resting_order_id && record.aggressive_side == wanted->aggressive_side &&
                record.buyer_id == wanted->buyer_id && record.seller_id == wanted->seller_id &&
                record.price == wanted->price && record.quantity == wanted->quantity;
        } else if constexpr (std::is_same_v<Record, OrderRejected> || std::is_same_v<Record, CancelRejected>) {
            return same_request(record.request, wanted->request) && record.reason == wanted->reason;
        } else {
            if (record.order_id != wanted->order_id || record.participant_id != wanted->participant_id) {
                return false;
            }
            if constexpr (std::is_same_v<Record, OrderCancelled>) {
                return record.price == wanted->price && record.cancelled_quantity == wanted->cancelled_quantity;
            } else if constexpr (std::is_same_v<Record, OrderRested>) {
                return record.price == wanted->price && record.remaining_quantity == wanted->remaining_quantity;
            } else {
                return record.remaining_quantity == wanted->remaining_quantity;
            }
        }
    }, actual);
}

struct Scenario {
    std::string_view name;
    std::vector<Command> preparation;
    Command request;
    std::vector<ExchangeEvent> events;
    // Best-price/FIFO order from the contract, with full original acceptance metadata.
    std::vector<RestingOrder> bids;
    std::vector<RestingOrder> asks;
};

std::vector<Scenario> scenarios() {
    const auto bid_five = limit(1, Side::Buy, 100, 5);
    const auto bid_two = limit(1, Side::Buy, 100, 2);
    const auto bid_three = limit(2, Side::Buy, 100, 3);
    const auto bid_four = limit(3, Side::Buy, 100, 4);
    const auto lower_bid = limit(2, Side::Buy, 99, 3);
    const auto ask_five = limit(1, Side::Sell, 101, 5);
    const auto ask_two = limit(1, Side::Sell, 101, 2);
    const auto ask_three = limit(2, Side::Sell, 101, 3);
    const auto ask_four = limit(3, Side::Sell, 101, 4);
    const auto higher_ask = limit(2, Side::Sell, 102, 3);
    const auto highest_ask = limit(3, Side::Sell, 103, 4);
    const auto buy_five = limit(10, Side::Buy, 100, 5, 2);
    const auto sell_five = limit(10, Side::Sell, 101, 5, 2);
    const auto sell_three = limit(10, Side::Sell, 101, 3, 2);
    const auto equality = limit(10, Side::Buy, 101, 5, 2);
    const auto small_buy = limit(10, Side::Buy, 102, 2, 2);
    const auto large_buy = limit(10, Side::Buy, 102, 5, 2);
    const auto fifo_buy = limit(10, Side::Buy, 101, 7, 2);
    const auto walk_asks = limit(10, Side::Buy, 102, 6, 2);
    const auto walk_bids = limit(10, Side::Sell, 99, 4, 2);
    const auto prior_fill = limit(11, Side::Buy, 101, 2, 2);
    const auto buy_market = market(10, Side::Buy, 5);
    const auto sell_market = market(10, Side::Sell, 3);
    const auto invalid_price = limit(10, Side::Buy, 0, 1, 2);
    const auto invalid_quantity = limit(10, Side::Buy, 100, 0, 2);
    const auto active_duplicate = limit(1, Side::Sell, 100, 1, 2);
    const auto terminal_duplicate = limit(1, Side::Buy, 100, 1, 2);

    return {
        {"Passive bid", {}, buy_five,
            {accepted(buy_five, header(1, 1)), rested(buy_five, 5, header(2, 1))},
            {resting(buy_five, 1, 5)}, {}},
        {"Passive ask", {}, sell_five,
            {accepted(sell_five, header(1, 1)), rested(sell_five, 5, header(2, 1))},
            {}, {resting(sell_five, 1, 5)}},
        {"Non-crossing sides", {bid_five}, sell_three,
            {accepted(sell_three, header(3, 2)), rested(sell_three, 3, header(4, 2))},
            {resting(bid_five, 1, 5)}, {resting(sell_three, 2, 3)}},
        {"Full crossing at equality", {ask_five}, equality,
            {accepted(equality, header(3, 2)), trade(equality, ask_five, 101, 5, header(4, 2)),
             filled(ask_five, header(5, 2)), filled(equality, header(6, 2))}, {}, {}},
        {"Partial resting fill", {ask_five}, small_buy,
            {accepted(small_buy, header(3, 2)), trade(small_buy, ask_five, 101, 2, header(4, 2)),
             partial(ask_five, 3, header(5, 2)), filled(small_buy, header(6, 2))},
            {}, {resting(ask_five, 1, 3)}},
        {"Aggressive remainder rests", {ask_two}, large_buy,
            {accepted(large_buy, header(3, 2)), trade(large_buy, ask_two, 101, 2, header(4, 2)),
             filled(ask_two, header(5, 2)), partial(large_buy, 3, header(6, 2)), rested(large_buy, 3, header(7, 2))},
            {resting(large_buy, 2, 3)}, {}},
        {"Three same-price FIFO orders", {ask_two, ask_three, ask_four}, fifo_buy,
            {accepted(fifo_buy, header(7, 4)), trade(fifo_buy, ask_two, 101, 2, header(8, 4)),
             filled(ask_two, header(9, 4)), partial(fifo_buy, 5, header(10, 4)),
             trade(fifo_buy, ask_three, 101, 3, header(11, 4)), filled(ask_three, header(12, 4)),
             partial(fifo_buy, 2, header(13, 4)), trade(fifo_buy, ask_four, 101, 2, header(14, 4)),
             partial(ask_four, 2, header(15, 4)), filled(fifo_buy, header(16, 4))},
            {}, {resting(ask_four, 3, 2)}},
        {"Walk ask levels", {ask_two, higher_ask, highest_ask}, walk_asks,
            {accepted(walk_asks, header(7, 4)), trade(walk_asks, ask_two, 101, 2, header(8, 4)),
             filled(ask_two, header(9, 4)), partial(walk_asks, 4, header(10, 4)),
             trade(walk_asks, higher_ask, 102, 3, header(11, 4)), filled(higher_ask, header(12, 4)),
             partial(walk_asks, 1, header(13, 4)), rested(walk_asks, 1, header(14, 4))},
            {resting(walk_asks, 4, 1)}, {resting(highest_ask, 3, 4)}},
        {"Walk bid levels", {bid_two, lower_bid}, walk_bids,
            {accepted(walk_bids, header(5, 3)), trade(walk_bids, bid_two, 100, 2, header(6, 3)),
             filled(bid_two, header(7, 3)), partial(walk_bids, 2, header(8, 3)),
             trade(walk_bids, lower_bid, 99, 2, header(9, 3)), partial(lower_bid, 1, header(10, 3)),
             filled(walk_bids, header(11, 3))}, {resting(lower_bid, 2, 1)}, {}},
        {"Cancel first", {bid_two, bid_three, bid_four}, cancel(1),
            {cancelled(bid_two, 2, header(7, 4))}, {resting(bid_three, 2, 3), resting(bid_four, 3, 4)}, {}},
        {"Cancel middle", {bid_two, bid_three, bid_four}, cancel(2),
            {cancelled(bid_three, 3, header(7, 4))}, {resting(bid_two, 1, 2), resting(bid_four, 3, 4)}, {}},
        {"Cancel final", {bid_two, bid_three, bid_four}, cancel(3),
            {cancelled(bid_four, 4, header(7, 4))}, {resting(bid_two, 1, 2), resting(bid_three, 2, 3)}, {}},
        {"Cancel only order", {ask_two}, cancel(1), {cancelled(ask_two, 2, header(3, 2))}, {}, {}},
        {"Cancel partially filled order", {ask_five, prior_fill}, cancel(1),
            {cancelled(ask_five, 3, header(7, 3))}, {}, {}},
        {"Cancel unknown or inactive", {bid_two}, cancel(99),
            {CancelRejected{header(3, 2), cancel(99), RejectionReason::NotActive}}, {resting(bid_two, 1, 2)}, {}},
        {"Cancel another owner's order", {bid_two}, cancel(1, 2),
            {CancelRejected{header(3, 2), cancel(1, 2), RejectionReason::NotOwner}}, {resting(bid_two, 1, 2)}, {}},
        {"Market with enough liquidity", {ask_five}, buy_market,
            {accepted(buy_market, header(3, 2)), trade(buy_market, ask_five, 101, 5, header(4, 2)),
             filled(ask_five, header(5, 2)), filled(buy_market, header(6, 2))}, {}, {}},
        {"Market with insufficient liquidity", {ask_two}, buy_market,
            {accepted(buy_market, header(3, 2)), trade(buy_market, ask_two, 101, 2, header(4, 2)),
             filled(ask_two, header(5, 2)), partial(buy_market, 3, header(6, 2)), expired(buy_market, 3, header(7, 2))}, {}, {}},
        {"Market against empty book", {}, sell_market,
            {accepted(sell_market, header(1, 1)), expired(sell_market, 3, header(2, 1))}, {}, {}},
        {"Invalid price", {bid_two}, invalid_price,
            {OrderRejected{header(3, 2), invalid_price, RejectionReason::InvalidPrice}}, {resting(bid_two, 1, 2)}, {}},
        {"Invalid quantity", {bid_two}, invalid_quantity,
            {OrderRejected{header(3, 2), invalid_quantity, RejectionReason::InvalidQuantity}}, {resting(bid_two, 1, 2)}, {}},
        {"Duplicate active ID", {bid_two}, active_duplicate,
            {OrderRejected{header(3, 2), active_duplicate, RejectionReason::DuplicateOrderId}}, {resting(bid_two, 1, 2)}, {}},
        {"Duplicate terminal ID", {bid_two, cancel(1)}, terminal_duplicate,
            {OrderRejected{header(4, 3), terminal_duplicate, RejectionReason::DuplicateOrderId}}, {}, {}},
    };
}

bool same_resting(const RestingOrder& actual, const RestingOrder& expected) {
    return marketlab::is_valid(actual) && same_order(actual.order, expected.order) &&
        actual.remaining_quantity == expected.remaining_quantity;
}

bool check_book(const OrderBook& book, const Scenario& scenario) {
    if (book.order_count() != scenario.bids.size() + scenario.asks.size() ||
        book.empty() != (scenario.bids.empty() && scenario.asks.empty())) {
        return false;
    }
    const auto check_side = [&](std::span<const RestingOrder> expected, const RestingOrder* head, Side side) {
        if (expected.empty()) {
            return head == nullptr;
        }
        if (!head || !same_resting(*head, expected.front())) {
            return false;
        }
        for (const auto& order : expected) {
            const auto* found = book.find(order.order.order_id);
            if (order.order.side != side || !book.contains(order.order.order_id) || !found || !same_resting(*found, order)) {
                return false;
            }
        }
        return true;
    };
    return check_side(scenario.bids, book.best_bid(), Side::Buy) &&
        check_side(scenario.asks, book.best_ask(), Side::Sell);
}

auto process(Exchange& exchange, const Command& command) {
    return std::visit([&](const auto& request) {
        if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
            return exchange.process_order(request);
        } else {
            return exchange.process_cancel(request);
        }
    }, command);
}

bool run_scenario(const Scenario& scenario) {
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    Exchange exchange{Instrument{{1}, "XYZ", TickSize::from_decimal(1, 2).value()}, participants};
    std::vector<std::vector<ExchangeEvent>> history;
    for (const auto& command : scenario.preparation) {
        auto result = process(exchange, command);
        if (!result || result->empty() ||
            (!std::holds_alternative<OrderAccepted>(result->front()) &&
             !std::holds_alternative<OrderCancelled>(result->front())) ||
            !std::ranges::all_of(*result, [](const auto& event) { return marketlab::is_valid(event); })) {
            std::cerr << scenario.name << ": preparation failed\n";
            return false;
        }
        history.emplace_back(std::move(*result));
    }
    const auto saved_history = history;
    const auto result = process(exchange, scenario.request);
    if (!result || result->size() != scenario.events.size()) {
        std::cerr << scenario.name << ": wrong event count or engine error\n";
        return false;
    }
    for (std::size_t index = 0; index < scenario.events.size(); ++index) {
        if (!same_event((*result)[index], scenario.events[index])) {
            std::cerr << scenario.name << ": event " << index + 1 << " differs\n";
            return false;
        }
    }
    const auto last = event_header(scenario.events.back());
    if (!check_book(exchange.book(), scenario) || exchange.current_time() != last.timestamp ||
        exchange.last_request_sequence() != last.request_sequence || exchange.last_event_sequence() != last.event_sequence) {
        std::cerr << scenario.name << ": final book, counters, or time differ\n";
        return false;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (!std::equal(history[index].begin(), history[index].end(),
                saved_history[index].begin(), saved_history[index].end(), same_event)) {
            std::cerr << scenario.name << ": prior returned events changed\n";
            return false;
        }
    }
    return true;
}

int main() {
    for (const auto& scenario : scenarios()) {
        if (!run_scenario(scenario)) {
            return 1;
        }
    }
}
