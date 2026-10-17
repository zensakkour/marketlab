#pragma once

#include <marketlab/event_record.hpp>
#include <marketlab/market_data.hpp>
#include <marketlab/participant_fills.hpp>
#include <marketlab/participant_plans.hpp>
#include <marketlab/participant_pnl.hpp>
#include <marketlab/scripted_participant.hpp>
#include <marketlab/time_weighted_spread.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <variant>
#include <vector>

namespace market_making_example {

using namespace marketlab;

struct RequestCounts {
    std::uint64_t orders{};
    std::uint64_t accepted_orders{};
    std::uint64_t rejected_orders{};
    std::uint64_t cancellations{};
    std::uint64_t cancelled{};
    std::uint64_t rejected_cancellations{};
    constexpr bool operator==(const RequestCounts&) const noexcept = default;
};

struct ParticipantResult {
    ParticipantId id{};
    RequestCounts requests{};
    ParticipantAccount account{};
    ParticipantFills fills{};
    PnlBreakdown pnl{};
    constexpr bool operator==(const ParticipantResult&) const noexcept = default;
};

struct Result {
    std::uint64_t seed;
    LogicalTime horizon;
    Price mark;
    std::vector<SimulationEvent> arrivals;
    std::vector<ExchangeEvent> events;
    DepthSnapshot market;
    SpreadStatistics spread;
    std::array<ParticipantResult, 4> participants;
    std::size_t pending_requests{};
};

// Finite reference workload. Varying the horizon clips delivery, not plan construction.
inline Result run(std::uint64_t seed, LogicalTime horizon = {500}) {
    constexpr std::array owners{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}, ParticipantId{4}};
    const auto reference = Price::from_ticks(100).value();
    SimulationRandom random{seed};
    OrderIdSource ids;
    const auto liquidity = make_liquidity_script({{1}, {1}, reference, 3, 8, {0}, 50, 11}, ids);
    const auto noise = make_noise_script(
        {{{1}, {2}, {5}, 100, 1, 9, 1, 4, 5000}, reference, 4, 2500}, random, ids);
    const auto taker = make_taker_script({{1}, {3}, {15}, 70, 4, 12, 1, 6, 5000}, random, ids);
    const auto quotes = make_liquidity_script({{1}, {4}, reference, 1, 5, {0}, 25, 21}, ids);
    if (!liquidity || !noise || !taker || !quotes) throw std::runtime_error{"reference plan generation failed"};
    ScriptedParticipant provider{{1}, *liquidity};
    ScriptedParticipant background{{2}, *noise};
    ScriptedParticipant consumer{{3}, *taker};
    ScriptedParticipant maker{{4}, *quotes};
    Simulation simulation{Instrument{{1}, "REFERENCE", TickSize::from_decimal(1, 2).value()}, owners};
    ParticipantLedger ledger{{1}, owners};
    // Actual scheduling insertion order resolves cross-participant timestamp ties.
    for (auto* actor : {&provider, &background, &consumer, &maker}) {
        while (!actor->empty()) {
            if (!actor->schedule_next(simulation)) throw std::runtime_error{"reference scheduling failed"};
        }
    }
    TimeWeightedSpread spread;
    Result result{seed, horizon, reference, {}, {}, {}, {}, {}, 0};
    while (const auto* pending = simulation.queue().next()) {
        const auto time = std::visit([](const auto& request) { return request.timestamp; }, pending->request);
        if (time >= horizon) break;
        const auto step = simulation.step();
        if (!step || !*step) throw std::runtime_error{"reference delivery failed"};
        std::vector<Execution> executions;
        for (const auto& event : step->value().events) {
            if (const auto* execution = std::get_if<Execution>(&event)) executions.push_back(*execution);
        }
        if (!ledger.apply(executions)) throw std::runtime_error{"reference accounting refused"};
        if (!simulation.exchange().book().check_invariants()) throw std::logic_error{"reference book invariant failed"};
        if (!spread.observe(time, best_prices(simulation.exchange()).spread_ticks())) {
            throw std::logic_error{"reference spread observation refused"};
        }
        result.arrivals.push_back(step->value().arrival);
        result.events.insert(result.events.end(), step->value().events.begin(), step->value().events.end());
    }
    result.pending_requests = simulation.queue().size();
    result.market = depth_snapshot(simulation.exchange(), 64);
    result.spread = spread.snapshot(horizon).value();
    for (std::size_t index = 0; index < owners.size(); ++index) {
        auto& participant = result.participants[index];
        participant.id = owners[index];
        participant.account = ledger.account(owners[index]).value();
        participant.fills = participant_fills(ledger, owners[index]).value();
        participant.pnl = fifo_pnl(ledger, owners[index], reference).value();
    }
    for (const auto& event : result.events) {
        if (const auto* accepted = std::get_if<OrderAccepted>(&event)) {
            auto& counts = result.participants.at(accepted->order.participant_id.value - 1).requests;
            ++counts.orders;
            ++counts.accepted_orders;
        } else if (const auto* rejected = std::get_if<OrderRejected>(&event)) {
            auto& counts = result.participants.at(rejected->request.participant_id.value - 1).requests;
            ++counts.orders;
            ++counts.rejected_orders;
        } else if (const auto* cancelled = std::get_if<OrderCancelled>(&event)) {
            auto& counts = result.participants.at(cancelled->participant_id.value - 1).requests;
            ++counts.cancellations;
            ++counts.cancelled;
        } else if (const auto* rejected_cancel = std::get_if<CancelRejected>(&event)) {
            auto& counts = result.participants.at(rejected_cancel->request.participant_id.value - 1).requests;
            ++counts.cancellations;
            ++counts.rejected_cancellations;
        }
    }
    return result;
}

inline void write_events(std::ostream& output, const Result& result) {
    for (const auto& event : result.events) write_event_record(output, event);
}

inline void write_summary(std::ostream& output, const Result& result) {
    std::uint64_t orders = 0, cancellations = 0, trades = 0;
    for (const auto& participant : result.participants) {
        orders += participant.requests.orders;
        cancellations += participant.requests.cancellations;
    }
    for (const auto& event : result.events) trades += std::holds_alternative<Execution>(event);
    const auto& market = result.market.market;
    const auto& prices = market.prices;
    output << "seed=" << result.seed << " instrument_id=" << prices.instrument_id.value
           << " mark_ticks=" << result.mark.ticks() << " horizon_ns=" << result.horizon.nanoseconds
           << " last_time_ns=" << prices.timestamp.nanoseconds << " requests=" << result.arrivals.size()
           << " pending_requests=" << result.pending_requests << " events=" << result.events.size()
           << " orders=" << orders << " cancellations=" << cancellations << " trades=" << trades
           << " volume_units=" << to_string(market.cumulative_volume) << " bid_ticks=";
    if (prices.bid) output << prices.bid->ticks(); else output << "none";
    output << " ask_ticks=";
    if (prices.ask) output << prices.ask->ticks(); else output << "none";
    output << '\n';
    output << "spread_tick_ns=" << to_string(result.spread.tick_nanoseconds)
           << " covered_ns=" << result.spread.covered_nanoseconds
           << " missing_ns=" << result.spread.missing_nanoseconds << " mean_spread_ticks=";
    const auto mean = result.spread.mean();
    if (mean) output << to_string(mean->tick_nanoseconds) << '/' << mean->covered_nanoseconds;
    else output << "none";
    output << '\n';
    constexpr std::array roles{"liquidity", "noise", "taker", "market_maker"};
    for (std::size_t index = 0; index < result.participants.size(); ++index) {
        const auto& participant = result.participants[index];
        const auto& counts = participant.requests;
        const auto& fills = participant.fills;
        const auto position = participant.account.position();
        const auto cash = participant.account.cash_flow();
        output << "participant_id=" << participant.id.value << " role=" << roles[index]
               << " orders=" << counts.orders << " accepted_orders=" << counts.accepted_orders
               << " rejected_orders=" << counts.rejected_orders << " cancellations=" << counts.cancellations
               << " cancelled=" << counts.cancelled << " rejected_cancellations=" << counts.rejected_cancellations
               << " buy_fills=" << fills.buy_count << " sell_fills=" << fills.sell_count
               << " aggressive_fills=" << fills.aggressive_count << " passive_fills=" << fills.passive_count
               << " position_units=";
        if (position.negative) output << '-';
        output << to_string(position.magnitude) << " cash_tick_units=";
        if (cash.negative) output << '-';
        output << to_string(cash.magnitude);
        const auto write_amount = [&](const char* name, PnlAmount amount) {
            output << ' ' << name << '=';
            if (amount.negative) output << '-';
            output << to_string(amount.magnitude);
        };
        write_amount("realised_pnl_tick_units", participant.pnl.realised);
        write_amount("unrealised_pnl_tick_units", participant.pnl.unrealised);
        write_amount("total_pnl_tick_units", participant.pnl.total);
        output << '\n';
    }
    const auto write_levels = [&](const char* name, const std::vector<DepthLevel>& levels) {
        output << name << '=';
        if (levels.empty()) output << "none";
        for (std::size_t index = 0; index < levels.size(); ++index) {
            if (index != 0) output << ',';
            output << levels[index].price.ticks() << ':' << to_string(levels[index].quantity)
                   << ':' << levels[index].order_count;
        }
    };
    write_levels("bids", result.market.depth.bids);
    output << ' ';
    write_levels("asks", result.market.depth.asks);
    output << '\n';
}

} // namespace market_making_example
