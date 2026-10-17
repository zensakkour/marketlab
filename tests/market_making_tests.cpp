#include "../examples/market_making_scenario.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace marketlab;
using market_making_example::Result;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

std::string event_text(const Result& result) {
    std::ostringstream output;
    market_making_example::write_events(output, result);
    return output.str();
}

std::string summary_text(const Result& result) {
    std::ostringstream output;
    market_making_example::write_summary(output, result);
    return output.str();
}

PnlAmount signed_amount(std::int64_t amount) {
    return {amount < 0, {0, 0, static_cast<std::uint64_t>(amount < 0 ? -amount : amount)}};
}

void reconcile(const Result& result) {
    struct Order { AcceptedOrder accepted; std::int64_t remaining; bool active{}; };
    struct Account {
        std::uint64_t bought{}, sold{}, spent{}, received{};
        market_making_example::RequestCounts requests{};
        ParticipantFills fills{};
        std::int64_t realised{};
        std::deque<std::int64_t> unit_lots;
    };
    std::array<Account, 4> accounts{};
    std::map<OrderId, Order> orders;
    std::uint64_t integral = 0, covered = 0, missing = 0, observation_time = 0, volume = 0;
    std::uint64_t sequence = 0;
    constexpr std::array owners{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}, ParticipantId{4}};
    Exchange replay{Instrument{{1}, "REFERENCE", TickSize::from_decimal(1, 2).value()}, owners};
    std::ostringstream replay_events;
    const auto account_index = [](ParticipantId id) {
        require(id.value >= 1 && id.value <= 4, "foreign reference participant");
        return static_cast<std::size_t>(id.value - 1);
    };
    const auto advance_observation = [&](std::uint64_t until) {
        std::optional<std::int64_t> bid, ask;
        for (const auto& [id, order] : orders) {
            static_cast<void>(id);
            if (!order.active) continue;
            const auto ticks = order.accepted.limit_price->ticks();
            if (order.accepted.side == Side::Buy && (!bid || ticks > *bid)) bid = ticks;
            if (order.accepted.side == Side::Sell && (!ask || ticks < *ask)) ask = ticks;
        }
        // A deliberately discrete independent oracle: one observation per nanosecond.
        while (observation_time < until) {
            if (bid && ask) {
                require(*ask > *bid, "independent reference book crossed");
                integral += static_cast<std::uint64_t>(*ask - *bid);
                ++covered;
            } else ++missing;
            ++observation_time;
        }
    };
    const auto apply_lots = [&](Account& account, bool buying, std::int64_t units, std::int64_t price) {
        for (std::int64_t unit = 0; unit < units; ++unit) {
            auto& lots = account.unit_lots;
            if (!lots.empty() && (lots.front() > 0) != buying) {
                account.realised += buying ? -lots.front() - price : price - lots.front();
                lots.pop_front();
            } else lots.push_back(buying ? price : -price);
        }
    };
    std::optional<LogicalTime> previous_time;
    SimulationSequence previous_sequence{};
    ParticipantId previous_owner{};
    for (std::size_t request_index = 0; request_index < result.arrivals.size(); ++request_index) {
        const auto& arrival = result.arrivals[request_index];
        const auto time = std::visit([](const auto& request) { return request.timestamp; }, arrival.request);
        const auto owner = std::visit([](const auto& request) { return request.participant_id; }, arrival.request);
        require(time < result.horizon && (!previous_time || time >= *previous_time) &&
            (!previous_time || time != *previous_time || arrival.sequence > previous_sequence),
            "reference horizon or equal-time insertion ordering differs");
        require(!previous_time || time != *previous_time || owner >= previous_owner,
            "reference cross-participant tie priority differs");
        previous_owner = owner;
        previous_time = time;
        previous_sequence = arrival.sequence;
        advance_observation(time.nanoseconds);
        const auto events = std::visit([&](const auto& request) {
            auto& counts = accounts[account_index(request.participant_id)].requests;
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                ++counts.orders;
                return replay.process_order(request);
            } else {
                ++counts.cancellations;
                return replay.process_cancel(request);
            }
        }, arrival.request);
        require(events.has_value(), "reference direct replay failed");
        for (const auto& event : *events) {
            write_event_record(replay_events, event);
            require(is_valid(event), "reference event invalid");
            std::visit([&](const auto& record) {
                using Record = std::decay_t<decltype(record)>;
                const auto event_sequence = [&] {
                    if constexpr (std::is_same_v<Record, Execution>) return record.event_sequence;
                    else return record.header.event_sequence;
                }();
                require(event_sequence.value == ++sequence, "reference event sequence has a gap");
                if constexpr (std::is_same_v<Record, OrderAccepted>) {
                    require(orders.emplace(record.order.order_id,
                        Order{record.order, record.order.original_quantity.units()}).second,
                        "reference accepted order ID repeated");
                    ++accounts[account_index(record.order.participant_id)].requests.accepted_orders;
                } else if constexpr (std::is_same_v<Record, Execution>) {
                    auto& incoming = orders.at(record.aggressive_order_id);
                    auto& resting = orders.at(record.resting_order_id);
                    const auto units = record.quantity.units();
                    require(units <= 8 && record.price.ticks() >= 96 && record.price.ticks() <= 104 &&
                        resting.active && incoming.remaining >= units && resting.remaining >= units &&
                        resting.accepted.limit_price == record.price && incoming.accepted.side == record.aggressive_side &&
                        incoming.accepted.side != resting.accepted.side, "reference execution state differs");
                    const auto buyer_id = incoming.accepted.side == Side::Buy ?
                        incoming.accepted.participant_id : resting.accepted.participant_id;
                    const auto seller_id = incoming.accepted.side == Side::Sell ?
                        incoming.accepted.participant_id : resting.accepted.participant_id;
                    require(buyer_id == record.buyer_id && seller_id == record.seller_id,
                        "reference counterparties differ");
                    incoming.remaining -= units;
                    resting.remaining -= units;
                    auto& buyer = accounts[account_index(buyer_id)];
                    auto& seller = accounts[account_index(seller_id)];
                    buyer.bought += static_cast<std::uint64_t>(units);
                    seller.sold += static_cast<std::uint64_t>(units);
                    const auto notional = static_cast<std::uint64_t>(units * record.price.ticks());
                    buyer.spent += notional;
                    seller.received += notional;
                    ++buyer.fills.buy_count;
                    ++seller.fills.sell_count;
                    ++accounts[account_index(incoming.accepted.participant_id)].fills.aggressive_count;
                    ++accounts[account_index(resting.accepted.participant_id)].fills.passive_count;
                    if (buyer_id != seller_id) {
                        apply_lots(buyer, true, units, record.price.ticks());
                        apply_lots(seller, false, units, record.price.ticks());
                    }
                    volume += static_cast<std::uint64_t>(units);
                } else if constexpr (std::is_same_v<Record, OrderRested>) {
                    auto& order = orders.at(record.order_id);
                    require(!order.active && order.remaining == record.remaining_quantity.units(),
                        "reference resting quantity differs");
                    order.active = true;
                } else if constexpr (std::is_same_v<Record, OrderPartiallyFilled>) {
                    require(orders.at(record.order_id).remaining == record.remaining_quantity.units(),
                        "reference partial quantity differs");
                } else if constexpr (std::is_same_v<Record, OrderFilled>) {
                    auto& order = orders.at(record.order_id);
                    require(order.remaining == 0, "reference filled order retained quantity");
                    order.active = false;
                } else if constexpr (std::is_same_v<Record, OrderCancelled>) {
                    auto& order = orders.at(record.order_id);
                    require(order.active && order.remaining == record.cancelled_quantity.units() &&
                        order.accepted.participant_id == record.participant_id, "reference cancel differs");
                    order.active = false;
                    order.remaining = 0;
                    ++accounts[account_index(record.participant_id)].requests.cancelled;
                } else if constexpr (std::is_same_v<Record, CancelRejected>) {
                    const auto found = orders.find(record.request.target_order_id);
                    require(record.reason == RejectionReason::NotActive &&
                        (found == orders.end() || !found->second.active), "reference cancel rejection differs");
                    ++accounts[account_index(record.request.participant_id)].requests.rejected_cancellations;
                } else if constexpr (std::is_same_v<Record, MarketRemainderExpired>) {
                    auto& order = orders.at(record.order_id);
                    require(order.remaining == record.remaining_quantity.units(), "reference expiry differs");
                    order.remaining = 0;
                } else if constexpr (std::is_same_v<Record, OrderRejected>) {
                    require(false, "valid reference plan produced rejected order");
                }
            }, event);
        }
    }
    advance_observation(result.horizon.nanoseconds);
    require(replay_events.str() == event_text(result) && result.arrivals.size() + result.pending_requests == 294,
        "reference event replay or total scheduled requests differs");
    require(result.spread == SpreadStatistics{{0, integral}, covered, missing} &&
        covered + missing == result.horizon.nanoseconds, "reference spread differs from unit-time oracle");
    require(result.market.market.cumulative_volume == AggregateQuantity{0, volume} &&
        result.market.market.prices.last_event_sequence.value == sequence &&
        result.market.market.prices.last_request_sequence.value == result.arrivals.size() &&
        result.market.market.prices.timestamp == replay.current_time(),
        "reference market metadata differs");
    std::int64_t net_pnl = 0;
    for (std::size_t index = 0; index < accounts.size(); ++index) {
        const auto& expected = accounts[index];
        const auto& actual = result.participants[index];
        require(actual.id == owners[index] && actual.requests == expected.requests && actual.fills == expected.fills &&
            actual.account == ParticipantAccount{{0, expected.bought}, {0, expected.sold},
                {0, 0, expected.spent}, {0, 0, expected.received}}, "reference account/count reconciliation differs");
        std::int64_t unrealised = 0;
        for (const auto entry : expected.unit_lots) {
            unrealised += entry > 0 ? result.mark.ticks() - entry : -entry - result.mark.ticks();
        }
        const auto total = static_cast<std::int64_t>(expected.received) - static_cast<std::int64_t>(expected.spent) +
            (static_cast<std::int64_t>(expected.bought) - static_cast<std::int64_t>(expected.sold)) * result.mark.ticks();
        require(actual.pnl == PnlBreakdown{signed_amount(expected.realised), signed_amount(unrealised), signed_amount(total)} &&
            expected.realised + unrealised == total, "reference FIFO PnL differs from unit-lot oracle");
        net_pnl += total;
    }
    require(net_pnl == 0, "reference common-mark total PnL not zero");
    std::map<std::int64_t, std::pair<std::uint64_t, std::size_t>> bids, asks;
    for (const auto& [id, order] : orders) {
        static_cast<void>(id);
        if (!order.active) continue;
        auto& level = (order.accepted.side == Side::Buy ? bids : asks)[order.accepted.limit_price->ticks()];
        level.first += static_cast<std::uint64_t>(order.remaining);
        ++level.second;
    }
    require(result.market.depth.bids.size() == bids.size() && result.market.depth.asks.size() == asks.size(),
        "reference depth size differs");
    std::size_t index = 0;
    for (auto level = bids.rbegin(); level != bids.rend(); ++level, ++index) {
        require(result.market.depth.bids[index].price.ticks() == level->first &&
            result.market.depth.bids[index].quantity == AggregateQuantity{0, level->second.first} &&
            result.market.depth.bids[index].order_count == level->second.second, "reference bid depth differs");
    }
    index = 0;
    for (const auto& level : asks) {
        require(result.market.depth.asks[index].price.ticks() == level.first &&
            result.market.depth.asks[index].quantity == AggregateQuantity{0, level.second.first} &&
            result.market.depth.asks[index].order_count == level.second.second, "reference ask depth differs");
        ++index;
    }
}

void test_literal_reference() {
    const auto result = market_making_example::run(42);
    const std::string expected =
        "seed=42 instrument_id=1 mark_ticks=100 horizon_ns=500 last_time_ns=496 requests=260 pending_requests=34 "
        "events=682 orders=186 cancellations=74 trades=99 volume_units=223 bid_ticks=97 ask_ticks=101\n"
        "spread_tick_ns=1078 covered_ns=500 missing_ns=0 mean_spread_ticks=1078/500\n"
        "participant_id=1 role=liquidity orders=20 accepted_orders=20 rejected_orders=0 cancellations=18 cancelled=18 "
        "rejected_cancellations=0 buy_fills=0 sell_fills=0 aggressive_fills=0 passive_fills=0 position_units=0 "
        "cash_tick_units=0 realised_pnl_tick_units=0 unrealised_pnl_tick_units=0 total_pnl_tick_units=0\n"
        "participant_id=2 role=noise orders=68 accepted_orders=68 rejected_orders=0 cancellations=18 cancelled=8 "
        "rejected_cancellations=10 buy_fills=40 sell_fills=22 aggressive_fills=5 passive_fills=57 position_units=39 "
        "cash_tick_units=-3754 realised_pnl_tick_units=61 unrealised_pnl_tick_units=85 total_pnl_tick_units=146\n"
        "participant_id=3 role=taker orders=58 accepted_orders=58 rejected_orders=0 cancellations=0 cancelled=0 "
        "rejected_cancellations=0 buy_fills=38 sell_fills=56 aggressive_fills=94 passive_fills=0 position_units=-46 "
        "cash_tick_units=4331 realised_pnl_tick_units=-197 unrealised_pnl_tick_units=-72 total_pnl_tick_units=-269\n"
        "participant_id=4 role=market_maker orders=40 accepted_orders=40 rejected_orders=0 cancellations=38 "
        "cancelled=21 rejected_cancellations=17 buy_fills=21 sell_fills=21 aggressive_fills=0 passive_fills=42 "
        "position_units=7 cash_tick_units=-577 realised_pnl_tick_units=116 unrealised_pnl_tick_units=7 "
        "total_pnl_tick_units=123\n"
        "bids=97:13:3,96:3:1 asks=101:2:1,102:4:2,103:25:11,104:5:3\n";
    require(summary_text(result) == expected, "reference literal summary changed");
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : event_text(result)) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    require(hash == 0xa5e016a02d517e53ULL, "reference v1 event checksum changed");
}

void test_horizon_and_refresh_priority() {
    const auto empty = market_making_example::run(42, {0});
    reconcile(empty);
    require(empty.arrivals.empty() && empty.events.empty() && !empty.spread.mean() &&
        empty.market.market.prices.timestamp == LogicalTime{0}, "zero horizon processed a request");
    const auto first = market_making_example::run(42, {1});
    reconcile(first);
    require(first.arrivals.size() == 4 && first.spread == SpreadStatistics{{0, 2}, 1, 0} &&
        first.market.market.prices.timestamp == LogicalTime{0}, "initial tie or horizon projection advanced exchange time");
    const auto drained = market_making_example::run(42, {1000});
    reconcile(drained);
    require(drained.pending_requests == 0 && drained.market.market.prices.timestamp.nanoseconds < 1000 &&
        drained.spread.covered_nanoseconds + drained.spread.missing_nanoseconds == 1000,
        "drained reference did not extend its last observation to the horizon");
    const auto exclusive = market_making_example::run(42);
    const auto inclusive_500 = market_making_example::run(42, {501});
    reconcile(inclusive_500);
    require(event_text(inclusive_500).starts_with(event_text(exclusive)) &&
        inclusive_500.arrivals.size() >= exclusive.arrivals.size() + 8, "500 ns refreshes not excluded from default");
    std::size_t at_boundary = 0, refresh_actions = 0;
    for (const auto& arrival : inclusive_500.arrivals) {
        const auto owner = std::visit([](const auto& request) { return request.participant_id; }, arrival.request);
        const auto time = std::visit([](const auto& request) { return request.timestamp; }, arrival.request);
        if (time == LogicalTime{500} && (owner == ParticipantId{1} || owner == ParticipantId{4})) ++at_boundary;
        if (time != LogicalTime{25} || owner != ParticipantId{4}) continue;
        require(std::holds_alternative<CancelRequest>(arrival.request) == (refresh_actions < 2),
            "maker refresh did not cancel before quoting");
        if (const auto* order = std::get_if<OrderRequest>(&arrival.request)) {
            require(order->side == (refresh_actions == 2 ? Side::Buy : Side::Sell),
                "maker refreshed ask before bid");
        }
        ++refresh_actions;
    }
    require(at_boundary == 8 && refresh_actions == 4, "reference boundary or maker refresh fixture missing");
}

int main() {
    try {
        test_literal_reference();
        test_horizon_and_refresh_priority();
        for (const auto seed : std::array<std::uint64_t, 4>{0, 42, 123, UINT64_MAX}) {
            const auto first = market_making_example::run(seed);
            reconcile(first);
            if (seed != 42) require(event_text(first) != event_text(market_making_example::run(42)),
                "reference ignored the configured seed");
            for (int repeat = 0; repeat < 3; ++repeat) {
                const auto replay = market_making_example::run(seed);
                require(event_text(first) == event_text(replay) && summary_text(first) == summary_text(replay) &&
                    first.participants == replay.participants && first.spread == replay.spread,
                    "reference same-seed replay changed output/state");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
