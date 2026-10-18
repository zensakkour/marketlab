#pragma once

#include <marketlab/event_record.hpp>
#include <marketlab/market_data.hpp>
#include <marketlab/participant_pnl.hpp>
#include <marketlab/partitioned_simulation.hpp>
#include <marketlab/threaded_simulation.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <map>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace partitioned_example {

using namespace marketlab;

enum class ExecutionMode { CallerDriven, Threaded };

struct InstrumentReport {
    Instrument instrument;
    MarketSnapshot market;
    Price mark;
    std::array<ParticipantAccount, 2> accounts;
    std::array<PnlBreakdown, 2> pnl;
};

struct Result {
    ExecutionMode mode{ExecutionMode::CallerDriven};
    std::size_t attempts{};
    std::size_t scheduled{};
    std::size_t refused{};
    std::vector<std::size_t> worker_steps;
    std::vector<ExchangeEvent> events;
    std::vector<InstrumentReport> instruments;
};

template<class Runner>
inline Result run_scenario(std::size_t workers) {
    if (workers == 0 || workers > 4) throw std::invalid_argument{"example requires 1 through 4 workers"};
    const std::array configs{
        Instrument{{1}, "ALPHA", TickSize::from_decimal(1, 2).value()},
        Instrument{{2}, "BETA", TickSize::from_decimal(5, 1).value()},
        Instrument{{3}, "GAMMA", TickSize::from_decimal(1, 0).value()}};
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    InstrumentRegistry registry{configs};
    std::vector<InstrumentWorkerAssignment> assignments;
    for (const auto& config : configs) assignments.push_back({config.id, (config.id.value - 1) % workers});
    WorkerAssignmentPlan plan{registry, workers, assignments};
    Runner runner{registry, plan, participants};
    std::map<InstrumentId, ParticipantLedger> ledgers;
    for (const auto& config : configs) ledgers.try_emplace(config.id, config.id, participants);
    const std::array<SimulationRequest, 12> script{
        OrderRequest{{10}, {1}, {1}, {1}, Side::Buy, OrderType::Limit, 5, 100},
        OrderRequest{{1}, {2}, {1}, {1}, Side::Sell, OrderType::Limit, 7, 200},
        OrderRequest{{5}, {3}, {1}, {1}, Side::Sell, OrderType::Limit, 3, 300},
        OrderRequest{{10}, {1}, {2}, {2}, Side::Sell, OrderType::Limit, 3, 100},
        OrderRequest{{2}, {2}, {2}, {2}, Side::Buy, OrderType::Market, 5, std::nullopt},
        OrderRequest{{5}, {3}, {2}, {2}, Side::Buy, OrderType::Limit, 4, 300},
        CancelRequest{{3}, {2}, {1}, {1}},
        CancelRequest{{11}, {1}, {1}, {1}},
        CancelRequest{{6}, {3}, {2}, {2}},
        OrderRequest{{12}, {1}, {1}, {3}, Side::Sell, OrderType::Market, 2, std::nullopt},
        OrderRequest{{4}, {2}, {2}, {3}, Side::Buy, OrderType::Limit, 0, 200},
        CancelRequest{{0}, {99}, {1}, {1}}};
    Result result;
    if constexpr (std::is_same_v<Runner, ThreadedSimulation>) result.mode = ExecutionMode::Threaded;
    result.worker_steps.resize(workers);
    for (const auto& request : script) {
        ++result.attempts;
        const auto scheduled = runner.schedule(request);
        if (scheduled) ++result.scheduled;
        else if (scheduled.error() == PartitionedScheduleError::UnknownInstrument) ++result.refused;
        else throw std::runtime_error{"partitioned example scheduling failed"};
    }
    const auto consume = [&](const WorkerStep& step) {
        ++result.worker_steps[step.worker_index];
        for (const auto& event : step.events) {
            if (const auto* execution = std::get_if<Execution>(&event)) {
                if (!ledgers.at(execution->instrument_id).apply(std::span{execution, 1})) {
                    throw std::logic_error{"partitioned example accounting refused"};
                }
            }
        }
        result.events.insert(result.events.end(), step.events.begin(), step.events.end());
    };
    if constexpr (std::is_same_v<Runner, ThreadedSimulation>) {
        const auto batches = runner.run();
        // This finite example reports only a complete successful experiment.
        // Library callers needing recovery retain all batches, including failed prefixes.
        for (const auto& batch : batches) {
            if (batch.exception) std::rethrow_exception(batch.exception);
            if (batch.error) throw std::runtime_error{"partitioned example engine failed"};
        }
        for (const auto& batch : batches) {
            for (const auto& step : batch.steps) consume(step);
        }
    } else {
        std::size_t processed = 0;
        while (processed < result.scheduled) {
            for (std::size_t worker = 0; worker < workers; ++worker) {
                const auto step = runner.step(worker);
                if (!step) throw std::runtime_error{"partitioned example engine failed"};
                if (!*step) continue;
                ++processed;
                consume(**step);
            }
        }
    }
    for (const auto& config : configs) {
        const auto& exchange = *runner.find(config.id);
        if (!exchange.book().check_invariants()) throw std::logic_error{"partitioned example book invariant failed"};
        const auto mark = Price::from_ticks(static_cast<std::int64_t>(config.id.value * 100 + 1)).value();
        const auto& ledger = ledgers.at(config.id);
        InstrumentReport report{exchange.instrument(), market_snapshot(exchange), mark, {}, {}};
        for (std::size_t index = 0; index < participants.size(); ++index) {
            report.accounts[index] = ledger.account(participants[index]).value();
            report.pnl[index] = fifo_pnl(ledger, participants[index], mark).value();
        }
        result.instruments.push_back(std::move(report));
    }
    return result;
}

inline Result run(std::size_t workers, ExecutionMode mode = ExecutionMode::CallerDriven) {
    switch (mode) {
    case ExecutionMode::CallerDriven: return run_scenario<PartitionedSimulation>(workers);
    case ExecutionMode::Threaded: return run_scenario<ThreadedSimulation>(workers);
    }
    throw std::invalid_argument{"unknown example execution mode"};
}

inline void write_events(std::ostream& output, const Result& result) {
    for (const auto& event : result.events) write_event_record(output, event);
}

inline void write_summary(std::ostream& output, const Result& result) {
    if (result.mode == ExecutionMode::Threaded) output << "execution=threaded ";
    output << "workers=" << result.worker_steps.size() << " attempts=" << result.attempts
           << " scheduled=" << result.scheduled << " refused=" << result.refused << " events=" << result.events.size() << '\n';
    for (std::size_t index = 0; index < result.worker_steps.size(); ++index) {
        output << "worker_index=" << index << " arrivals=" << result.worker_steps[index] << '\n';
    }
    for (const auto& report : result.instruments) {
        const auto& prices = report.market.prices;
        output << "instrument_id=" << report.instrument.id.value << " symbol=" << report.instrument.symbol
               << " time_ns=" << prices.timestamp.nanoseconds << " requests=" << prices.last_request_sequence.value
               << " events=" << prices.last_event_sequence.value << " volume_units=" << to_string(report.market.cumulative_volume)
               << " bid_ticks=";
        if (prices.bid) output << prices.bid->ticks(); else output << "none";
        output << " ask_ticks=";
        if (prices.ask) output << prices.ask->ticks(); else output << "none";
        output << '\n';
        for (std::size_t index = 0; index < report.accounts.size(); ++index) {
            const auto position = report.accounts[index].position();
            const auto cash = report.accounts[index].cash_flow();
            const auto pnl = report.pnl[index].total;
            output << "instrument_id=" << report.instrument.id.value << " participant_id=" << index + 1
                   << " mark_ticks=" << report.mark.ticks() << " position_units=";
            if (position.negative) output << '-';
            output << to_string(position.magnitude) << " cash_tick_units=";
            if (cash.negative) output << '-';
            output << to_string(cash.magnitude) << " total_pnl_tick_units=";
            if (pnl.negative) output << '-';
            output << to_string(pnl.magnitude) << '\n';
        }
    }
}

} // namespace partitioned_example
