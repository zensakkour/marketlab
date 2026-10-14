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

int main() {
    try {
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
