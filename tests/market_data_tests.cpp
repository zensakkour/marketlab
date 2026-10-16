#include <marketlab/market_data.hpp>
#include <marketlab/simulation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

using namespace marketlab;

static_assert(noexcept(best_prices(std::declval<const Exchange&>())));
static_assert(!std::is_convertible_v<MidPrice, Price>);
static_assert(noexcept(market_snapshot(std::declval<const Exchange&>())));
static_assert(noexcept(std::declval<const Exchange&>().last_trade()));
static_assert(noexcept(std::declval<const BestPrices&>().spread_ticks()));
static_assert(noexcept(std::declval<const BestPrices&>().mid_price()));
constexpr BestPrices half_tick{{1}, {0}, {0}, {0}, Price::from_ticks(100), Price::from_ticks(101)};
static_assert(half_tick.spread_ticks() == 1);
static_assert(half_tick.mid_price() == MidPrice{100, true});
constexpr BestPrices missing_side{{1}, {0}, {0}, {0}, Price::from_ticks(100), std::nullopt};
static_assert(!missing_side.spread_ticks() && !missing_side.mid_price());

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

OrderRequest limit(std::uint64_t id, Side side, std::int64_t ticks, std::int64_t units,
    std::uint64_t time = 0, std::uint64_t participant = 1) {
    return {{time}, {1}, {participant}, {id}, side, OrderType::Limit, units, ticks};
}

void check_prices(const Exchange& exchange, std::optional<std::int64_t> bid, std::optional<std::int64_t> ask) {
    const auto snapshot = best_prices(exchange);
    const auto value = [](const std::optional<Price>& price) -> std::optional<std::int64_t> {
        return price ? std::optional{price->ticks()} : std::nullopt;
    };
    require(snapshot.instrument_id == exchange.book().instrument_id() &&
        snapshot.timestamp == exchange.current_time() && snapshot.last_request_sequence == exchange.last_request_sequence() &&
        snapshot.last_event_sequence == exchange.last_event_sequence(), "quote snapshot metadata differs");
    require(value(snapshot.bid) == bid && value(snapshot.ask) == ask, "quote snapshot prices differ");
    if (bid && ask) {
        const auto distance = *ask - *bid;
        require(snapshot.spread_ticks() == distance &&
            snapshot.mid_price() == MidPrice{*bid + distance / 2, distance % 2 != 0},
            "quote spread or exact midpoint differs");
    } else {
        require(!snapshot.spread_ticks() && !snapshot.mid_price(), "one-sided quote has a spread or midpoint");
    }
    require(exchange.book().check_invariants(), "quote read changed book invariants");
}

void test_price_transitions() {
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    Exchange exchange{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    check_prices(exchange, {}, {});
    const auto initial = best_prices(exchange);
    require(exchange.process_order(limit(900, Side::Buy, 100, 5, 1)).has_value(), "initial bid failed");
    check_prices(exchange, 100, {});
    const auto bid_only = best_prices(exchange);
    require(exchange.process_order(limit(1, Side::Buy, 100, 3, 1, 2)).has_value(), "same-price bid failed");
    check_prices(exchange, 100, {});
    require(exchange.process_order(limit(2, Side::Sell, 101, 5, 2, 2)).has_value(), "initial ask failed");
    check_prices(exchange, 100, 101);
    const auto two_sided = best_prices(exchange);
    require(exchange.process_order(limit(3, Side::Sell, 103, 4, 2, 2)).has_value(), "worse ask failed");
    check_prices(exchange, 100, 101);
    require(exchange.process_order(limit(50, Side::Buy, 101, 2, 3)).has_value(), "partial fill failed");
    check_prices(exchange, 100, 101);
    require(exchange.book().find({2})->remaining_quantity.units() == 3, "partial fill remainder differs");
    require(exchange.process_order(limit(51, Side::Buy, 101, 3, 3)).has_value(), "head fill failed");
    check_prices(exchange, 100, 103);
    require(!exchange.book().find({2}), "filled ask remained active");
    require(exchange.process_order(limit(4, Side::Sell, 102, 2, 4, 2)).has_value(), "better ask failed");
    check_prices(exchange, 100, 102);
    require(exchange.process_order(limit(52, Side::Buy, 103, 3, 4)).has_value(), "multi-level fill failed");
    check_prices(exchange, 100, 103);
    require(!exchange.book().find({4}) && exchange.book().find({3})->remaining_quantity.units() == 3,
        "multi-level remainders differ");
    require(exchange.process_cancel(CancelRequest{{5}, {1}, {2}, {3}}).has_value(), "last ask cancel failed");
    check_prices(exchange, 100, {});
    require(exchange.process_cancel(CancelRequest{{5}, {1}, {1}, {900}}).has_value(), "head bid cancel failed");
    check_prices(exchange, 100, {});
    require(exchange.book().best_bid()->order.order_id.value == 1, "same-price survivor lost FIFO");
    require(exchange.process_cancel(CancelRequest{{6}, {1}, {2}, {1}}).has_value(), "last bid cancel failed");
    check_prices(exchange, {}, {});
    require(!initial.bid && !initial.ask && initial.timestamp.nanoseconds == 0 &&
        initial.last_request_sequence.value == 0 && initial.last_event_sequence.value == 0,
        "initial owned snapshot changed");
    require(bid_only.bid->ticks() == 100 && !bid_only.ask && bid_only.timestamp.nanoseconds == 1 &&
        bid_only.last_request_sequence.value == 1 && bid_only.last_event_sequence.value == 2,
        "one-sided owned snapshot changed");
    require(two_sided.bid->ticks() == 100 && two_sided.ask->ticks() == 101 &&
        two_sided.timestamp.nanoseconds == 2 && two_sided.last_request_sequence.value == 3 &&
        two_sided.last_event_sequence.value == 6, "two-sided owned snapshot changed");
}

void test_bid_price_transitions() {
    const std::array participants{ParticipantId{1}};
    Exchange exchange{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    require(exchange.process_order(limit(1, Side::Buy, 90, 2)).has_value(), "lower bid failed");
    require(exchange.process_order(limit(2, Side::Buy, 100, 5)).has_value(), "higher bid failed");
    require(exchange.process_order(limit(3, Side::Buy, 95, 3)).has_value(), "middle bid failed");
    require(exchange.process_order(limit(4, Side::Sell, 101, 2)).has_value(), "ask preparation failed");
    check_prices(exchange, 100, 101);
    const auto before = best_prices(exchange);
    const OrderRequest sell{{1}, {1}, {1}, {50}, Side::Sell, OrderType::Market, 6, std::nullopt};
    const auto filled = exchange.process_order(sell);
    require(filled.has_value(), "market sell across bid levels failed");
    check_prices(exchange, 95, 101);
    require(!exchange.book().find({2}) && exchange.book().find({3})->remaining_quantity.units() == 2,
        "market sell bid remainders differ");
    require(exchange.process_cancel(CancelRequest{{2}, {1}, {1}, {3}}).has_value(), "best bid cancel failed");
    check_prices(exchange, 90, 101);
    require(exchange.process_cancel(CancelRequest{{2}, {1}, {1}, {1}}).has_value(), "final bid cancel failed");
    check_prices(exchange, {}, 101);
    require(before.bid->ticks() == 100 && before.ask->ticks() == 101 && before.timestamp.nanoseconds == 0 &&
        before.last_request_sequence.value == 4 && before.last_event_sequence.value == 8,
        "saved snapshot changed across bid levels");
}

void test_rejections_and_boundaries() {
    const std::array participants{ParticipantId{1}};
    Exchange exchange{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    require(exchange.process_order(limit(1, Side::Buy, 1, maximum, 10)).has_value(), "minimum bid failed");
    require(exchange.process_order(limit(2, Side::Sell, maximum, maximum, 10)).has_value(), "maximum ask failed");
    check_prices(exchange, 1, maximum);
    auto invalid = limit(3, Side::Buy, 2, 0, 20);
    const auto rejected = exchange.process_order(invalid);
    require(rejected && rejected->size() == 1 && std::holds_alternative<OrderRejected>(rejected->front()),
        "invalid order was not rejected");
    check_prices(exchange, 1, maximum);
    const auto after_rejection = best_prices(exchange);
    require(after_rejection.timestamp.nanoseconds == 20 && after_rejection.last_request_sequence.value == 3 &&
        after_rejection.last_event_sequence.value == 5, "rejected request metadata did not advance");
    invalid.timestamp = {19};
    const auto backward = exchange.process_order(invalid);
    require(backward && std::get<OrderRejected>(backward->front()).reason == RejectionReason::InvalidTimestamp,
        "backward order was not rejected");
    check_prices(exchange, 1, maximum);
    require(exchange.current_time().nanoseconds == 20, "backward request advanced time");
    const auto inactive = exchange.process_cancel(CancelRequest{{20}, {1}, {1}, {99}});
    require(inactive && std::get<CancelRejected>(inactive->front()).reason == RejectionReason::NotActive,
        "inactive cancel was not rejected");
    check_prices(exchange, 1, maximum);
    require(exchange.process_cancel(CancelRequest{{20}, {1}, {1}, {1}}).has_value(), "boundary bid cancel failed");
    check_prices(exchange, {}, maximum);
    const auto ask_only = best_prices(exchange);
    require(ask_only.ask->ticks() == maximum && !ask_only.bid, "one-sided maximum price lost");
}

void test_exact_midpoints() {
    struct Case {
        std::int64_t bid;
        std::int64_t ask;
        std::int64_t spread;
        MidPrice mid;
    };
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::array cases{
        Case{1, 2, 1, {1, true}},
        Case{1, 3, 2, {2, false}},
        Case{100, 101, 1, {100, true}},
        Case{100, 102, 2, {101, false}},
        Case{1, maximum, maximum - 1, {maximum / 2 + 1, false}},
        Case{2, maximum, maximum - 2, {maximum / 2 + 1, true}},
        Case{maximum - 2, maximum, 2, {maximum - 1, false}},
        Case{maximum - 1, maximum, 1, {maximum - 1, true}},
    };
    const std::array participants{ParticipantId{1}};
    for (const auto& test : cases) {
        Exchange exchange{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
        require(exchange.process_order(limit(1, Side::Buy, test.bid, 1)).has_value(), "midpoint bid failed");
        require(exchange.process_order(limit(2, Side::Sell, test.ask, 1)).has_value(), "midpoint ask failed");
        const auto snapshot = best_prices(exchange);
        require(snapshot.spread_ticks() == test.spread && snapshot.mid_price() == test.mid,
            "midpoint boundary or half-tick precision differs");
        require(exchange.process_cancel(CancelRequest{{0}, {1}, {1}, {2}}).has_value(), "midpoint ask removal failed");
        const auto one_sided = best_prices(exchange);
        require(!one_sided.spread_ticks() && !one_sided.mid_price(), "midpoint survived removal of one side");
        require(snapshot.spread_ticks() == test.spread && snapshot.mid_price() == test.mid,
            "derived values changed in a retained snapshot");
    }
}

void test_simulation_read_boundaries() {
    const std::array participants{ParticipantId{1}};
    Simulation simulation{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    const auto& exchange = simulation.exchange();
    require(simulation.schedule(limit(2, Side::Sell, 101, 5, 2)).has_value(), "scheduled ask failed");
    require(simulation.schedule(limit(1, Side::Buy, 100, 3, 1)).has_value(), "scheduled bid failed");
    check_prices(exchange, {}, {});
    require(best_prices(exchange).last_request_sequence.value == 0 && simulation.queue().last_sequence().value == 2,
        "pending arrivals leaked into committed snapshot");
    for (std::size_t index = 0; index < 2; ++index) {
        const auto step = simulation.step();
        require(step && *step, "scheduled quote delivery failed");
        check_prices(exchange, 100, index == 0 ? std::nullopt : std::optional<std::int64_t>{101});
    }
    const auto settled = best_prices(exchange);
    const auto empty = simulation.step();
    require(empty && !*empty && settled.timestamp.nanoseconds == 2 && settled.last_request_sequence.value == 2 &&
        settled.last_event_sequence.value == 4, "empty step changed snapshot metadata");
    require(simulation.schedule(CancelRequest{{2}, {1}, {1}, {2}}).has_value(), "scheduled quote cancel failed");
    check_prices(exchange, 100, 101);
    const auto cancellation = simulation.step();
    require(cancellation && *cancellation, "quote cancel delivery failed");
    check_prices(exchange, 100, {});
    require(settled.bid->ticks() == 100 && settled.ask->ticks() == 101 && settled.last_event_sequence.value == 4,
        "previous simulation snapshot changed after cancel");
}

bool same_trade(const Execution& a, const Execution& b) {
    return a.timestamp == b.timestamp && a.event_sequence == b.event_sequence &&
        a.request_sequence == b.request_sequence && a.instrument_id == b.instrument_id &&
        a.aggressive_order_id == b.aggressive_order_id && a.resting_order_id == b.resting_order_id &&
        a.aggressive_side == b.aggressive_side && a.buyer_id == b.buyer_id && a.seller_id == b.seller_id &&
        a.price == b.price && a.quantity == b.quantity;
}

void test_last_trade() {
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    Exchange exchange{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    require(!exchange.last_trade() && !market_snapshot(exchange).last_trade, "initial last trade is present");
    require(exchange.process_order(limit(1, Side::Sell, 101, 2, 1, 2)).has_value(), "first ask failed");
    require(exchange.process_order(limit(2, Side::Sell, 102, 5, 1, 2)).has_value(), "second ask failed");
    require(!exchange.last_trade(), "passive order created a trade");
    const auto first = exchange.process_order(limit(3, Side::Buy, 102, 4, 2));
    require(first && first->size() == 7, "two-level fill failed");
    const Execution expected{{2}, {9}, {3}, {1}, {3}, {2}, Side::Buy, {1}, {2},
        Price::from_ticks(102).value(), Quantity::from_units(2).value()};
    const auto saved = market_snapshot(exchange);
    require(saved.last_trade && same_trade(*saved.last_trade, expected) &&
        same_trade(*exchange.last_trade(), expected), "last trade is not the final execution");
    require(saved.prices.timestamp.nanoseconds == 2 && saved.prices.last_event_sequence.value == 11 &&
        saved.prices.ask->ticks() == 102 && !saved.prices.bid, "trade snapshot metadata or prices differ");
    require(exchange.process_cancel(CancelRequest{{3}, {1}, {2}, {2}}).has_value(), "trade remainder cancel failed");
    const OrderRequest expired{{4}, {1}, {1}, {4}, Side::Buy, OrderType::Market, 1, std::nullopt};
    require(exchange.process_order(expired).has_value(), "empty market failed");
    const auto rejected = exchange.process_order(limit(5, Side::Buy, 100, 0, 5));
    require(rejected && std::holds_alternative<OrderRejected>(rejected->front()), "invalid order not rejected");
    require(exchange.process_cancel(CancelRequest{{6}, {1}, {1}, {99}}).has_value(), "inactive cancel failed");
    require(exchange.process_order(limit(6, Side::Buy, 100, 1, 5)).has_value(), "backward rejection failed");
    const auto later = market_snapshot(exchange);
    require(later.last_trade && same_trade(*later.last_trade, expected) &&
        later.prices.timestamp.nanoseconds == 6 && later.last_trade->timestamp.nanoseconds == 2 &&
        !later.prices.bid && !later.prices.ask, "non-trading requests changed last trade");

    // Exact maximum values and the permitted self-match retain both counterparties.
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    require(exchange.process_order(limit(7, Side::Buy, maximum, maximum, 6)).has_value(), "maximum bid failed");
    const OrderRequest sell{{6}, {1}, {1}, {8}, Side::Sell, OrderType::Market, maximum, std::nullopt};
    const auto second = exchange.process_order(sell);
    require(second && exchange.last_trade(), "self-match market sell failed");
    const auto& final_execution = std::get<Execution>((*second)[1]);
    require(same_trade(*exchange.last_trade(), final_execution) &&
        final_execution.aggressive_side == Side::Sell && final_execution.price.ticks() == maximum &&
        final_execution.quantity.units() == maximum && final_execution.buyer_id == final_execution.seller_id,
        "last trade lost sell, self-match or boundary fields");
    require(saved.last_trade && same_trade(*saved.last_trade, expected) && saved.prices.ask->ticks() == 102,
        "owned trade snapshot changed after later requests");
    require(exchange.book().check_invariants(), "trade projection changed book invariants");

    const auto detached = [&] {
        Exchange temporary{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
        require(temporary.process_order(limit(1, Side::Buy, 1, 1)).has_value(), "detached bid failed");
        require(temporary.process_order(limit(2, Side::Sell, 1, 1)).has_value(), "detached trade failed");
        return market_snapshot(temporary);
    }();
    require(detached.last_trade && detached.last_trade->price.ticks() == 1 &&
        detached.last_trade->quantity.units() == 1, "trade snapshot depends on source lifetime");
}

void test_scheduled_trade() {
    const std::array participants{ParticipantId{1}};
    Simulation simulation{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    require(simulation.schedule(limit(1, Side::Sell, 100, 3, 1)).has_value() &&
        simulation.schedule(limit(2, Side::Buy, 100, 2, 2)).has_value() &&
        simulation.schedule(limit(3, Side::Buy, 100, 1, 2)).has_value(), "trade scheduling failed");
    require(!market_snapshot(simulation.exchange()).last_trade, "future trade leaked into snapshot");
    require(simulation.step().has_value() && !simulation.exchange().last_trade(), "passive step created trade");
    require(simulation.step().has_value(), "first trade step failed");
    const auto first = market_snapshot(simulation.exchange());
    require(first.last_trade && first.last_trade->quantity.units() == 2 &&
        first.last_trade->timestamp.nanoseconds == 2, "first scheduled trade differs");
    require(simulation.step().has_value(), "second trade step failed");
    const auto second = market_snapshot(simulation.exchange());
    require(second.last_trade && second.last_trade->timestamp == first.last_trade->timestamp &&
        second.last_trade->event_sequence > first.last_trade->event_sequence &&
        second.last_trade->quantity.units() == 1 && first.last_trade->quantity.units() == 2,
        "equal-time trades lost event ordering or snapshot ownership");
    const auto empty = simulation.step();
    require(empty && !*empty && same_trade(*simulation.exchange().last_trade(), *second.last_trade),
        "empty step changed last trade");
}

int main() {
    try {
        test_last_trade();
        test_scheduled_trade();
        test_price_transitions();
        test_bid_price_transitions();
        test_rejections_and_boundaries();
        test_exact_midpoints();
        test_simulation_read_boundaries();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
