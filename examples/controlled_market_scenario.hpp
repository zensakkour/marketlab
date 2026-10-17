#pragma once

#include <marketlab/event_record.hpp>
#include <marketlab/market_data.hpp>
#include <marketlab/participant_fills.hpp>
#include <marketlab/participant_plans.hpp>
#include <marketlab/participant_pnl.hpp>
#include <marketlab/scripted_participant.hpp>

#include <array>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace controlled_market_example {

using namespace marketlab;

struct Result {
    std::uint64_t seed;
    Price mark;
    std::vector<ExchangeEvent> events;
    DepthSnapshot market;
    std::array<ParticipantAccount, 3> accounts;
    std::array<PnlBreakdown, 3> pnl;
    std::array<ParticipantFills, 3> fills;
};

inline Result run(std::uint64_t seed) {
    constexpr std::array participants{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}};
    const auto reference = Price::from_ticks(100).value();
    SimulationRandom random{seed};
    OrderIdSource ids;
    const auto liquidity = make_liquidity_script(
        {{1}, {1}, reference, 2, 8, {0}, 50, 8}, ids);
    const auto noise = make_noise_script(
        {{{1}, {2}, {5}, 60, 1, 9, 1, 4, 5000}, reference, 4, 2500}, random, ids);
    const auto taker = make_taker_script({{1}, {3}, {15}, 30, 4, 12, 1, 6, 5000}, random, ids);
    if (!liquidity || !noise || !taker) throw std::runtime_error{"participant plan generation failed"};
    ScriptedParticipant provider{{1}, *liquidity};
    ScriptedParticipant background{{2}, *noise};
    ScriptedParticipant consumer{{3}, *taker};
    Simulation simulation{Instrument{{1}, "CONTROL", TickSize::from_decimal(1, 2).value()}, participants};
    ParticipantLedger ledger{{1}, participants};
    // Plan construction order assigns IDs. Scheduling order resolves cross-participant ties.
    for (auto* actor : {&provider, &background, &consumer}) {
        while (!actor->empty()) {
            if (!actor->schedule_next(simulation)) throw std::runtime_error{"participant scheduling failed"};
        }
    }
    Result result{seed, reference, {}, {}, {}, {}, {}};
    for (;;) {
        const auto step = simulation.step();
        if (!step) throw std::runtime_error{"simulation capacity exhausted"};
        if (!*step) break;
        std::vector<Execution> executions;
        for (const auto& event : step->value().events) {
            if (const auto* execution = std::get_if<Execution>(&event)) executions.push_back(*execution);
        }
        if (!ledger.apply(executions)) throw std::runtime_error{"execution accounting refused"};
        if (!simulation.exchange().book().check_invariants()) throw std::logic_error{"book invariant failed"};
        result.events.insert(result.events.end(), step->value().events.begin(), step->value().events.end());
    }
    result.market = depth_snapshot(simulation.exchange(), 64);
    for (std::size_t index = 0; index < participants.size(); ++index) {
        result.accounts[index] = ledger.account(participants[index]).value();
        result.pnl[index] = fifo_pnl(ledger, participants[index], reference).value();
        result.fills[index] = participant_fills(ledger, participants[index]).value();
    }
    return result;
}

inline void write_events(std::ostream& output, const Result& result) {
    for (const auto& event : result.events) write_event_record(output, event);
}

inline void write_fills(std::ostream& output, const Result& result) {
    for (std::size_t index = 0; index < result.fills.size(); ++index) {
        const auto& fills = result.fills[index];
        output << "participant_id=" << index + 1
               << " buy_fills=" << fills.buy_count << " sell_fills=" << fills.sell_count
               << " aggressive_fills=" << fills.aggressive_count << " passive_fills=" << fills.passive_count << '\n';
    }
}

inline void write_summary(std::ostream& output, const Result& result) {
    std::size_t orders = 0;
    std::size_t cancellations = 0;
    std::size_t executions = 0;
    for (const auto& event : result.events) {
        orders += std::holds_alternative<OrderAccepted>(event) || std::holds_alternative<OrderRejected>(event);
        cancellations += std::holds_alternative<OrderCancelled>(event) || std::holds_alternative<CancelRejected>(event);
        executions += std::holds_alternative<Execution>(event);
    }
    const auto& market = result.market.market;
    const auto& prices = market.prices;
    output << "seed=" << result.seed << " instrument_id=" << prices.instrument_id.value
           << " mark_ticks=" << result.mark.ticks() << " requests=" << prices.last_request_sequence.value
           << " events=" << result.events.size() << " orders=" << orders << " cancellations=" << cancellations
           << " executions=" << executions << " time_ns=" << prices.timestamp.nanoseconds
           << " volume_units=" << to_string(market.cumulative_volume) << " bid_ticks=";
    if (prices.bid) output << prices.bid->ticks(); else output << "none";
    output << " ask_ticks=";
    if (prices.ask) output << prices.ask->ticks(); else output << "none";
    output << '\n';
    for (std::size_t index = 0; index < result.accounts.size(); ++index) {
        const auto position = result.accounts[index].position();
        const auto cash = result.accounts[index].cash_flow();
        output << "participant_id=" << index + 1 << " position_units=";
        if (position.negative) output << '-';
        output << to_string(position.magnitude) << " cash_tick_units=";
        if (cash.negative) output << '-';
        output << to_string(cash.magnitude);
        const auto write_amount = [&](const char* name, PnlAmount amount) {
            output << ' ' << name << '=';
            if (amount.negative) output << '-';
            output << to_string(amount.magnitude);
        };
        write_amount("realised_pnl_tick_units", result.pnl[index].realised);
        write_amount("unrealised_pnl_tick_units", result.pnl[index].unrealised);
        write_amount("total_pnl_tick_units", result.pnl[index].total);
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

} // namespace controlled_market_example
