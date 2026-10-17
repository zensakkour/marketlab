#include "../examples/controlled_market_scenario.hpp"
#include "participant_test_support.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

using namespace participant_test;
using controlled_market_example::Result;

std::string event_text(const Result& result) {
    std::ostringstream output;
    controlled_market_example::write_events(output, result);
    return output.str();
}

std::string summary_text(const Result& result) {
    std::ostringstream output;
    controlled_market_example::write_summary(output, result);
    return output.str();
}

void reconcile(const Result& result) {
    struct Order {
        AcceptedOrder order;
        std::int64_t remaining;
    };
    std::map<OrderId, Order> orders;
    std::set<OrderId> active;
    std::array<std::uint64_t, 3> bought{}, sold{}, spent{}, received{};
    std::uint64_t volume = 0;
    std::uint64_t sequence = 0;
    for (const auto& event : result.events) {
        require(is_valid(event), "integrated event is invalid");
        std::visit([&](const auto& record) {
            using Record = std::decay_t<decltype(record)>;
            const auto event_sequence = [&] {
                if constexpr (std::is_same_v<Record, Execution>) return record.event_sequence;
                else return record.header.event_sequence;
            }();
            require(event_sequence.value == ++sequence, "integrated event sequence has a gap");
            if constexpr (std::is_same_v<Record, OrderAccepted>) {
                require(orders.emplace(record.order.order_id, Order{record.order, record.order.original_quantity.units()}).second,
                    "integrated accepted ID duplicated");
            } else if constexpr (std::is_same_v<Record, Execution>) {
                auto& incoming = orders.at(record.aggressive_order_id);
                auto& resting = orders.at(record.resting_order_id);
                const auto units = record.quantity.units();
                require(active.contains(record.resting_order_id) && incoming.remaining >= units &&
                    resting.remaining >= units && resting.order.limit_price == record.price &&
                    (incoming.order.side == Side::Buy ? incoming.order.participant_id : resting.order.participant_id) == record.buyer_id &&
                    (incoming.order.side == Side::Sell ? incoming.order.participant_id : resting.order.participant_id) == record.seller_id,
                    "integrated execution disagrees with independent live orders");
                incoming.remaining -= units;
                resting.remaining -= units;
                const auto buyer = static_cast<std::size_t>(record.buyer_id.value - 1);
                const auto seller = static_cast<std::size_t>(record.seller_id.value - 1);
                bought[buyer] += static_cast<std::uint64_t>(units);
                sold[seller] += static_cast<std::uint64_t>(units);
                const auto notional = static_cast<std::uint64_t>(units * record.price.ticks());
                spent[buyer] += notional;
                received[seller] += notional;
                volume += static_cast<std::uint64_t>(units);
            } else if constexpr (std::is_same_v<Record, OrderRested>) {
                require(orders.at(record.order_id).remaining == record.remaining_quantity.units() &&
                    active.insert(record.order_id).second, "integrated resting state differs");
            } else if constexpr (std::is_same_v<Record, OrderPartiallyFilled>) {
                require(orders.at(record.order_id).remaining == record.remaining_quantity.units(),
                    "integrated partial remainder differs");
            } else if constexpr (std::is_same_v<Record, OrderFilled>) {
                require(orders.at(record.order_id).remaining == 0, "integrated full fill retained quantity");
                active.erase(record.order_id);
            } else if constexpr (std::is_same_v<Record, OrderCancelled>) {
                require(active.erase(record.order_id) == 1 &&
                    orders.at(record.order_id).remaining == record.cancelled_quantity.units(),
                    "integrated cancellation differs");
                orders.at(record.order_id).remaining = 0;
            } else if constexpr (std::is_same_v<Record, MarketRemainderExpired>) {
                require(orders.at(record.order_id).remaining == record.remaining_quantity.units(), "integrated expiry differs");
                orders.at(record.order_id).remaining = 0;
            } else if constexpr (std::is_same_v<Record, CancelRejected>) {
                require(record.reason == RejectionReason::NotActive && !active.contains(record.request.target_order_id),
                    "integrated cancellation rejection differs");
            } else if constexpr (std::is_same_v<Record, OrderRejected>) {
                require(false, "generated plan produced a rejected new order");
            }
        }, event);
    }
    require(result.market.market.cumulative_volume == AggregateQuantity{0, volume} &&
        result.market.market.prices.last_event_sequence.value == sequence &&
        result.market.market.prices.last_request_sequence.value == 120, "integrated market metadata differs");
    std::int64_t net_total = 0;
    for (std::size_t index = 0; index < result.accounts.size(); ++index) {
        require(result.accounts[index] == ParticipantAccount{{0, bought[index]}, {0, sold[index]},
            {0, 0, spent[index]}, {0, 0, received[index]}}, "integrated account differs from execution reconciliation");
        const auto expected = static_cast<std::int64_t>(received[index]) - static_cast<std::int64_t>(spent[index]) +
            (static_cast<std::int64_t>(bought[index]) - static_cast<std::int64_t>(sold[index])) * result.mark.ticks();
        net_total += expected;
        const auto magnitude = static_cast<std::uint64_t>(expected < 0 ? -expected : expected);
        require(result.pnl[index].total == PnlAmount{expected < 0, {0, 0, magnitude}},
            "integrated total PnL differs from independent cash and quantities");
    }
    require(net_total == 0, "integrated common-mark PnL does not reconcile");

    std::map<std::int64_t, std::pair<std::uint64_t, std::size_t>> bids, asks;
    for (const auto id : active) {
        const auto& order = orders.at(id);
        auto& level = (order.order.side == Side::Buy ? bids : asks)[order.order.limit_price->ticks()];
        level.first += static_cast<std::uint64_t>(order.remaining);
        ++level.second;
    }
    require(result.market.depth.bids.size() == bids.size() && result.market.depth.asks.size() == asks.size(),
        "integrated final depth size differs");
    std::size_t index = 0;
    for (auto level = bids.rbegin(); level != bids.rend(); ++level, ++index) {
        const auto& actual = result.market.depth.bids[index];
        require(actual.price.ticks() == level->first && actual.quantity == AggregateQuantity{0, level->second.first} &&
            actual.order_count == level->second.second, "integrated bid depth differs");
    }
    index = 0;
    for (const auto& level : asks) {
        const auto& actual = result.market.depth.asks[index++];
        require(actual.price.ticks() == level.first && actual.quantity == AggregateQuantity{0, level.second.first} &&
            actual.order_count == level.second.second, "integrated ask depth differs");
    }
}

int main() {
    try {
        const auto golden = controlled_market_example::run(42);
        reconcile(golden);
        const std::string expected =
            "seed=42 instrument_id=1 mark_ticks=100 requests=120 events=317 orders=93 cancellations=27 executions=45 time_ns=350 volume_units=91 bid_ticks=99 ask_ticks=101\n"
            "participant_id=1 position_units=8 cash_tick_units=-720 realised_pnl_tick_units=64 unrealised_pnl_tick_units=16 total_pnl_tick_units=80\n"
            "participant_id=2 position_units=5 cash_tick_units=-445 realised_pnl_tick_units=51 unrealised_pnl_tick_units=4 total_pnl_tick_units=55\n"
            "participant_id=3 position_units=-13 cash_tick_units=1165 realised_pnl_tick_units=-121 unrealised_pnl_tick_units=-14 total_pnl_tick_units=-135\n"
            "bids=99:4:2,98:10:2,97:19:7,96:3:1 asks=101:1:1,102:8:1,103:6:5,104:5:3\n";
        require(summary_text(golden) == expected, "controlled-market golden summary changed");
        for (const auto seed : std::array<std::uint64_t, 4>{0, 42, 123, UINT64_MAX}) {
            const auto first = controlled_market_example::run(seed);
            reconcile(first);
            for (int repeat = 0; repeat < 3; ++repeat) {
                const auto replay = controlled_market_example::run(seed);
                require(event_text(first) == event_text(replay) && summary_text(first) == summary_text(replay) &&
                    first.accounts == replay.accounts && first.pnl == replay.pnl,
                    "controlled-market same-seed events or final state differ");
            }
            if (seed != 42) require(event_text(first) != event_text(golden), "different seed did not affect generated flow");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
