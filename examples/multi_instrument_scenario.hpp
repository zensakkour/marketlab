#pragma once

#include <marketlab/event_record.hpp>
#include <marketlab/market_data.hpp>
#include <marketlab/multi_instrument_exchange.hpp>
#include <marketlab/participant_fills.hpp>
#include <marketlab/participant_pnl.hpp>
#include <marketlab/simulation_queue.hpp>

#include <array>
#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace multi_instrument_example {

using namespace marketlab;

struct InstrumentResult {
    Instrument instrument;
    Price mark;
    MarketSnapshot market;
    std::array<ParticipantAccount, 2> accounts;
    std::array<ParticipantFills, 2> fills;
    std::array<PnlBreakdown, 2> pnl;
};

struct Result {
    std::size_t arrivals{};
    std::size_t routing_errors{};
    std::vector<ExchangeEvent> events;
    std::vector<InstrumentResult> instruments;
};

inline Result run() {
    const std::array configs{
        Instrument{{2}, "BETA", TickSize::from_decimal(5, 1).value()},
        Instrument{{1}, "ALPHA", TickSize::from_decimal(1, 2).value()}};
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    InstrumentRegistry registry{configs};
    MultiInstrumentExchange exchange{registry, participants};
    ParticipantLedger alpha{{1}, participants}, beta{{2}, participants};
    SimulationQueue arrivals;
    const std::array<SimulationRequest, 12> script{
        OrderRequest{{0}, {1}, {1}, {1}, Side::Sell, OrderType::Limit, 5, 100},
        OrderRequest{{0}, {2}, {1}, {1}, Side::Sell, OrderType::Limit, 7, 200},
        OrderRequest{{1}, {1}, {2}, {2}, Side::Buy, OrderType::Limit, 3, 101},
        OrderRequest{{1}, {2}, {2}, {2}, Side::Buy, OrderType::Limit, 4, 201},
        CancelRequest{{2}, {1}, {1}, {1}},
        CancelRequest{{2}, {2}, {2}, {1}},
        OrderRequest{{3}, {2}, {2}, {3}, Side::Buy, OrderType::Market, 5, std::nullopt},
        OrderRequest{{3}, {1}, {2}, {3}, Side::Buy, OrderType::Limit, 4, 99},
        OrderRequest{{4}, {99}, {1}, {4}, Side::Buy, OrderType::Limit, 1, 100},
        CancelRequest{{5}, {1}, {2}, {3}},
        CancelRequest{{5}, {2}, {1}, {1}},
        OrderRequest{{6}, {1}, {1}, {4}, Side::Sell, OrderType::Market, 2, std::nullopt}};
    for (const auto& request : script) {
        if (!arrivals.schedule(request)) throw std::runtime_error{"multi-instrument scheduling failed"};
    }
    Result result;
    while (const auto* pending = arrivals.next()) {
        const auto events = std::visit([&](const auto& request) {
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                return exchange.process_order(request);
            } else return exchange.process_cancel(request);
        }, pending->request);
        if (!events && events.error() != MultiInstrumentError::UnknownInstrument) {
            throw std::runtime_error{"multi-instrument engine capacity exhausted"};
        }
        // This caller counts/discards unknown destinations. No exchange event is fabricated.
        static_cast<void>(arrivals.pop_next());
        ++result.arrivals;
        if (!events) {
            ++result.routing_errors;
            continue;
        }
        std::vector<Execution> executions;
        for (const auto& event : *events) {
            if (const auto* execution = std::get_if<Execution>(&event)) executions.push_back(*execution);
        }
        if (!executions.empty()) {
            auto& ledger = executions.front().instrument_id == InstrumentId{1} ? alpha : beta;
            if (!ledger.apply(executions)) throw std::logic_error{"multi-instrument accounting refused"};
        }
        result.events.insert(result.events.end(), events->begin(), events->end());
        for (const auto id : {InstrumentId{1}, InstrumentId{2}}) {
            if (!exchange.find(id)->book().check_invariants()) throw std::logic_error{"multi-instrument invariant failed"};
        }
    }
    for (const auto id : {InstrumentId{1}, InstrumentId{2}}) {
        const auto& instrument_exchange = *exchange.find(id);
        const auto& ledger = id == InstrumentId{1} ? alpha : beta;
        const auto mark = Price::from_ticks(id == InstrumentId{1} ? 101 : 201).value();
        InstrumentResult report{instrument_exchange.instrument(), mark, market_snapshot(instrument_exchange), {}, {}, {}};
        for (std::size_t index = 0; index < participants.size(); ++index) {
            report.accounts[index] = ledger.account(participants[index]).value();
            report.fills[index] = participant_fills(ledger, participants[index]).value();
            report.pnl[index] = fifo_pnl(ledger, participants[index], mark).value();
        }
        result.instruments.push_back(std::move(report));
    }
    return result;
}

inline void write_events(std::ostream& output, const Result& result) {
    for (const auto& event : result.events) write_event_record(output, event);
}

inline void write_summary(std::ostream& output, const Result& result) {
    output << "arrivals=" << result.arrivals << " routing_errors=" << result.routing_errors
           << " events=" << result.events.size() << '\n';
    for (const auto& report : result.instruments) {
        const auto& prices = report.market.prices;
        output << "instrument_id=" << report.instrument.id.value << " symbol=" << report.instrument.symbol
               << " tick_coefficient=" << report.instrument.tick_size.coefficient()
               << " tick_scale=" << static_cast<unsigned>(report.instrument.tick_size.scale())
               << " time_ns=" << prices.timestamp.nanoseconds
               << " requests=" << prices.last_request_sequence.value << " events=" << prices.last_event_sequence.value
               << " volume_units=" << to_string(report.market.cumulative_volume);
        if (report.market.last_trade) {
            output << " last_trade_ticks=" << report.market.last_trade->price.ticks()
                   << " last_trade_units=" << report.market.last_trade->quantity.units();
        } else output << " last_trade_ticks=none last_trade_units=none";
        output << " bid_ticks=";
        if (prices.bid) output << prices.bid->ticks(); else output << "none";
        output << " ask_ticks=";
        if (prices.ask) output << prices.ask->ticks(); else output << "none";
        output << '\n';
        for (std::size_t index = 0; index < report.accounts.size(); ++index) {
            const auto& account = report.accounts[index];
            const auto position = account.position();
            const auto cash = account.cash_flow();
            const auto& fills = report.fills[index];
            output << "instrument_id=" << report.instrument.id.value << " participant_id=" << index + 1
                   << " mark_ticks=" << report.mark.ticks() << " buy_fills=" << fills.buy_count
                   << " sell_fills=" << fills.sell_count << " aggressive_fills=" << fills.aggressive_count
                   << " passive_fills=" << fills.passive_count << " position_units=";
            if (position.negative) output << '-';
            output << to_string(position.magnitude) << " cash_tick_units=";
            if (cash.negative) output << '-';
            output << to_string(cash.magnitude);
            const auto write_amount = [&](const char* name, PnlAmount amount) {
                output << ' ' << name << '=';
                if (amount.negative) output << '-';
                output << to_string(amount.magnitude);
            };
            write_amount("realised_pnl_tick_units", report.pnl[index].realised);
            write_amount("unrealised_pnl_tick_units", report.pnl[index].unrealised);
            write_amount("total_pnl_tick_units", report.pnl[index].total);
            output << '\n';
        }
    }
}

} // namespace multi_instrument_example
