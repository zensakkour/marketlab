#include "multi_instrument_test_support.hpp"
#include "../examples/multi_instrument_scenario.hpp"

#include <marketlab/participant_pnl.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace multi_test;

static_assert(!std::is_copy_constructible_v<MultiInstrumentExchange>);
static_assert(!std::is_move_constructible_v<MultiInstrumentExchange>);
static_assert(std::is_same_v<decltype(std::declval<MultiInstrumentExchange&>().find({1})), const Exchange*>);
static_assert(std::is_same_v<decltype(std::declval<Exchange&>().instrument()), const Instrument&>);
static_assert(noexcept(std::declval<const MultiInstrumentExchange&>().find({1})));
static_assert(noexcept(std::declval<const Exchange&>().instrument()));
static_assert(detail::multi_instrument_error(ExchangeError::RequestSequenceExhausted) ==
    MultiInstrumentError::RequestSequenceExhausted);
static_assert(detail::multi_instrument_error(ExchangeError::EventSequenceExhausted) ==
    MultiInstrumentError::EventSequenceExhausted);
static_assert(detail::multi_instrument_error(ExchangeError::OutputCapacityExceeded) ==
    MultiInstrumentError::OutputCapacityExceeded);

void test_configuration_and_borrowed_views() {
    std::optional<MultiInstrumentExchange> owner;
    {
        auto configs = configurations();
        InstrumentRegistry registry{configs};
        std::array ids{ParticipantId{1}, ParticipantId{1}, ParticipantId{2}};
        owner.emplace(registry, ids);
        configs[1].symbol = "CHANGED";
        configs[1].tick_size = TickSize::from_decimal(1, 0).value();
        ids.fill(ParticipantId{99});
    }
    auto& exchange = *owner;
    require(exchange.size() == 2 && !exchange.empty() && !exchange.find({0}) && !exchange.find({99}),
        "multi-instrument lookup/configuration differs");
    const auto* first = exchange.find({1});
    const auto* metadata = &first->instrument();
    require(metadata->symbol == "ALPHA" && metadata->id == InstrumentId{1} &&
        metadata->tick_size.coefficient() == 1 && metadata->tick_size.scale() == 2 &&
        exchange.find({2})->instrument().tick_size.coefficient() == 5 &&
        exchange.find({2})->instrument().tick_size.scale() == 1, "owner borrowed registry/caller configuration");
    const auto accepted = exchange.process_order(order(1, 1, 1, Side::Buy, 100, 2, 1));
    const auto unknown_participant = exchange.process_order(order(2, 1, 99, Side::Buy, 200, 2, 1));
    require(accepted && std::holds_alternative<OrderAccepted>(accepted->front()) && unknown_participant &&
        std::get<OrderRejected>(unknown_participant->front()).reason == RejectionReason::InvalidParticipant &&
        exchange.find({1}) == first && &first->instrument() == metadata && metadata->symbol == "ALPHA",
        "fixed participants, retained configuration or exchange view changed");

    InstrumentRegistry empty;
    MultiInstrumentExchange none{empty, participants};
    require(none.empty() && none.size() == 0 && !none.find({1}) &&
        none.process_order(order(1, 1, 1, Side::Buy, 100, 1, 0)).error() == MultiInstrumentError::UnknownInstrument &&
        none.process_cancel(cancel(1, 1, 1, 0)).error() == MultiInstrumentError::UnknownInstrument,
        "empty router did not refuse unknown instrument");
    const auto registered_configs = configurations();
    InstrumentRegistry registered{registered_configs};
    for (const auto* registry : {&empty, &registered}) {
        const std::array invalid{ParticipantId{0}};
        bool refused = false;
        try { MultiInstrumentExchange invalid_owner{*registry, invalid}; }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused, "empty instrument set bypassed participant validation");
    }
}

void test_maximum_identity_and_empty_participants() {
    const std::array configs{Instrument{{UINT64_MAX}, "MAX", TickSize::from_decimal(1, 0).value()}};
    InstrumentRegistry registry{configs};
    MultiInstrumentExchange exchange{registry, participants};
    const auto request = order(UINT64_MAX, UINT64_MAX, 1, Side::Buy, INT64_MAX, INT64_MAX, UINT64_MAX);
    const auto accepted = exchange.process_order(request);
    const auto cancelled = exchange.process_cancel(cancel(UINT64_MAX, UINT64_MAX, 1, UINT64_MAX));
    require(accepted && std::holds_alternative<OrderAccepted>(accepted->front()) && cancelled &&
        std::get<OrderCancelled>(cancelled->front()).cancelled_quantity.units() == INT64_MAX &&
        exchange.find({UINT64_MAX})->book().empty() &&
        exchange.find({UINT64_MAX})->instrument().id == InstrumentId{UINT64_MAX},
        "maximum routed instrument/order IDs or values differ");
    MultiInstrumentExchange no_participants{registry, {}};
    const auto rejected = no_participants.process_order(request);
    require(rejected && std::get<OrderRejected>(rejected->front()).reason == RejectionReason::InvalidParticipant,
        "known instrument bypassed an empty participant set");
}

void test_isolation_and_independent_accounting() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    MultiInstrumentExchange routed{registry, participants};
    Exchange baseline_a{*registry.find({1}), participants};
    Exchange baseline_b{*registry.find({2}), participants};
    ParticipantLedger ledger_a{{1}, participants}, ledger_b{{2}, participants};
    auto expired = order(1, 5, 2, Side::Buy, 100, 2, 12);
    expired.type = OrderType::Market;
    expired.limit_price_ticks.reset();
    auto taking_b = order(2, 6, 1, Side::Buy, 200, 5, 5);
    taking_b.type = OrderType::Market;
    taking_b.limit_price_ticks.reset();
    const std::vector<SimulationRequest> requests{
        order(1, 1, 1, Side::Sell, 100, 5, 10),
        order(2, 1, 1, Side::Buy, 200, 7, 1),
        order(1, 2, 1, Side::Sell, 100, 3, 10),
        order(2, 2, 2, Side::Sell, 200, 4, 1),
        order(1, 3, 2, Side::Buy, 101, 6, 10),
        cancel(2, 1, 1, 2), cancel(1, 2, 2, 10), cancel(1, 2, 1, 11),
        order(2, 1, 1, Side::Buy, 200, 2, 3), order(1, 1, 1, Side::Buy, 100, 2, 11),
        order(1, 4, 1, Side::Buy, 100, 0, 9), order(2, 4, 99, Side::Buy, 190, 2, 4),
        expired, order(2, 5, 2, Side::Sell, 195, 5, 4),
        order(1, 6, 1, Side::Buy, 50, 2, 13), taking_b, cancel(2, 6, 1, 5), cancel(1, 6, 1, 13)};
    const auto* a = routed.find({1});
    const auto* b = routed.find({2});
    const auto* a_config = &a->instrument();
    const auto* b_config = &b->instrument();
    for (const auto& request : requests) {
        const auto instrument = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const bool first = instrument == InstrumentId{1};
        const auto other_before = state(first ? *b : *a);
        auto& baseline = first ? baseline_a : baseline_b;
        auto& ledger = first ? ledger_a : ledger_b;
        auto& other_ledger = first ? ledger_b : ledger_a;
        const auto other_account = other_ledger.account({1});
        const auto* other_history = other_ledger.history().data();
        const auto result = std::visit([&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, OrderRequest>) return routed.process_order(value);
            else return routed.process_cancel(value);
        }, request);
        const auto expected = std::visit([&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, OrderRequest>) return baseline.process_order(value);
            else return baseline.process_cancel(value);
        }, request);
        require(result && expected && event_text(*result) == event_text(*expected) &&
            state(first ? *a : *b) == state(baseline) && state(first ? *b : *a) == other_before,
            "routed result differs from isolated exchange or mutated another instrument");
        std::vector<Execution> executions;
        for (const auto& event : *result) {
            if (const auto* execution = std::get_if<Execution>(&event)) executions.push_back(*execution);
        }
        if (!executions.empty()) {
            const auto foreign = other_ledger.apply(executions);
            require(!foreign && foreign.error() == AccountingError::WrongInstrument,
                "foreign instrument executions entered another ledger");
        }
        require(ledger.apply(executions).has_value() && other_ledger.account({1}) == other_account &&
            other_ledger.history().data() == other_history, "account ingestion changed another instrument");
        require(a == routed.find({1}) && b == routed.find({2}) &&
            a_config == &a->instrument() && b_config == &b->instrument() &&
            a->book().check_invariants() && b->book().check_invariants(), "routing invalidated views/book invariants");
    }
    require(a->current_time() == LogicalTime{13} && b->current_time() == LogicalTime{5} &&
        a->cumulative_volume() == AggregateQuantity{0, 6} && b->cumulative_volume() == AggregateQuantity{0, 9},
        "instrument clocks/volume were combined");
    require(ledger_a.account({2}) == ParticipantAccount{{0, 6}, {}, {0, 0, 600}, {}} &&
        ledger_b.account({1}) == ParticipantAccount{{0, 9}, {}, {0, 0, 1775}, {}} &&
        ledger_a.account({2})->total_pnl(Price::from_ticks(101).value()) == TotalPnl{false, {0, 0, 6}} &&
        ledger_b.account({1})->total_pnl(Price::from_ticks(201).value()) == TotalPnl{false, {0, 0, 34}},
        "instrument cash/valuation did not reconcile independently");
    for (const auto unknown : {InstrumentId{0}, InstrumentId{99}, InstrumentId{UINT64_MAX}}) {
        const auto before_a = state(*a), before_b = state(*b);
        auto invalid = order(unknown.value, 1, 0, Side::Buy, -1, -1, UINT64_MAX);
        const auto new_order = routed.process_order(invalid);
        const auto cancellation = routed.process_cancel(cancel(unknown.value, 1, 0, UINT64_MAX));
        require(!new_order && new_order.error() == MultiInstrumentError::UnknownInstrument &&
            !cancellation && cancellation.error() == MultiInstrumentError::UnknownInstrument &&
            state(*a) == before_a && state(*b) == before_b, "unknown routing consumed an instrument event or clock");
    }
}

void test_scripted_example() {
    const std::string expected =
        "arrivals=12 routing_errors=1 events=25\n"
        "instrument_id=1 symbol=ALPHA tick_coefficient=1 tick_scale=2 time_ns=6 requests=6 events=12 volume_units=3 "
        "last_trade_ticks=100 last_trade_units=3 bid_ticks=none ask_ticks=none\n"
        "instrument_id=1 participant_id=1 mark_ticks=101 buy_fills=0 sell_fills=1 aggressive_fills=0 passive_fills=1 "
        "position_units=-3 cash_tick_units=300 realised_pnl_tick_units=0 unrealised_pnl_tick_units=-3 "
        "total_pnl_tick_units=-3\n"
        "instrument_id=1 participant_id=2 mark_ticks=101 buy_fills=1 sell_fills=0 aggressive_fills=1 passive_fills=0 "
        "position_units=3 cash_tick_units=-300 realised_pnl_tick_units=0 unrealised_pnl_tick_units=3 "
        "total_pnl_tick_units=3\n"
        "instrument_id=2 symbol=BETA tick_coefficient=5 tick_scale=1 time_ns=5 requests=5 events=13 volume_units=7 "
        "last_trade_ticks=200 last_trade_units=3 bid_ticks=none ask_ticks=none\n"
        "instrument_id=2 participant_id=1 mark_ticks=201 buy_fills=0 sell_fills=2 aggressive_fills=0 passive_fills=2 "
        "position_units=-7 cash_tick_units=1400 realised_pnl_tick_units=0 unrealised_pnl_tick_units=-7 "
        "total_pnl_tick_units=-7\n"
        "instrument_id=2 participant_id=2 mark_ticks=201 buy_fills=2 sell_fills=0 aggressive_fills=2 passive_fills=0 "
        "position_units=7 cash_tick_units=-1400 realised_pnl_tick_units=0 unrealised_pnl_tick_units=7 "
        "total_pnl_tick_units=7\n";
    std::string first_events;
    for (int repeat = 0; repeat < 20; ++repeat) {
        const auto result = multi_instrument_example::run();
        std::ostringstream summary, events;
        multi_instrument_example::write_summary(summary, result);
        multi_instrument_example::write_events(events, result);
        require(summary.str() == expected && result.arrivals == 12 && result.routing_errors == 1 &&
            result.events.size() == 25 && result.instruments.size() == 2, "multi-instrument example summary changed");
        if (repeat == 0) first_events = events.str();
        require(events.str() == first_events, "multi-instrument example event replay changed");
        std::uint64_t hash = 14695981039346656037ULL;
        for (const unsigned char byte : events.str()) hash = (hash ^ byte) * 1099511628211ULL;
        require(hash == 0x9e74fbb840f4cf87ULL, "multi-instrument v1 event checksum changed");
        require(result.instruments[0].market.prices.timestamp == LogicalTime{6} &&
            result.instruments[1].market.prices.timestamp == LogicalTime{5} &&
            result.instruments[0].market.prices.last_event_sequence.value == 12 &&
            result.instruments[1].market.prices.last_event_sequence.value == 13,
            "merged recording replaced instrument-local metadata");
    }
}

int main() {
    try {
        test_configuration_and_borrowed_views();
        test_maximum_identity_and_empty_participants();
        test_scripted_example();
        test_isolation_and_independent_accounting();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
