#include "instrument_workload.hpp"

#include <marketlab/participant_pnl.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <map>
#include <type_traits>

using namespace multi_test;

void compare_workload(std::uint64_t seed, std::size_t symbols, std::size_t requests) {
    const auto configs = workload_instruments(symbols);
    InstrumentRegistry registry{configs};
    MultiInstrumentExchange routed{registry, participants};
    std::map<InstrumentId, Exchange> isolated;
    std::map<InstrumentId, ParticipantLedger> routed_accounts, isolated_accounts;
    for (const auto& config : configs) {
        isolated.try_emplace(config.id, config, participants);
        routed_accounts.try_emplace(config.id, config.id, participants);
        isolated_accounts.try_emplace(config.id, config.id, participants);
    }
    std::size_t trades = 0, rejections = 0, cancellations = 0;
    const auto script = instrument_workload(seed, symbols, requests);
    for (std::size_t index = 0; index < script.size(); ++index) {
        const auto& request = script[index];
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        auto& baseline = isolated.at(id);
        const auto result = std::visit([&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, OrderRequest>) return routed.process_order(value);
            else return routed.process_cancel(value);
        }, request);
        const auto expected = std::visit([&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, OrderRequest>) return baseline.process_order(value);
            else return baseline.process_cancel(value);
        }, request);
        if (!result || !expected || event_text(*result) != event_text(*expected) ||
            state(*routed.find(id), 64) != state(baseline, 64)) {
            throw std::runtime_error{"multi-instrument mismatch: seed=" + std::to_string(seed) +
                " symbols=" + std::to_string(symbols) + " request=" + std::to_string(index + 1)};
        }
        require(routed_accounts.at(id).apply(executions_in(*result)).has_value() &&
            isolated_accounts.at(id).apply(executions_in(*expected)).has_value(), "generated ledger refused execution");
        for (const auto& event : *result) {
            trades += std::holds_alternative<Execution>(event);
            rejections += std::holds_alternative<OrderRejected>(event) || std::holds_alternative<CancelRejected>(event);
            cancellations += std::holds_alternative<OrderCancelled>(event);
        }
        require(routed.find(id)->book().check_invariants(), "generated routed book invariant failed");
    }
    const auto mark = Price::from_ticks(100).value();
    for (const auto& config : configs) {
        const auto& actual = *routed.find(config.id);
        const auto& baseline = isolated.at(config.id);
        require(state(actual, 64) == state(baseline, 64) && actual.book().check_invariants(),
            "generated final state changed another instrument");
        for (const auto participant : participants) {
            const auto& accounts = routed_accounts.at(config.id);
            const auto& expected_accounts = isolated_accounts.at(config.id);
            require(accounts.account(participant) == expected_accounts.account(participant) &&
                fifo_pnl(accounts, participant, mark) == fifo_pnl(expected_accounts, participant, mark),
                "generated per-instrument account/PnL differs");
        }
    }
    require(trades != 0 && rejections != 0 && cancellations != 0, "generated workload lacks category coverage");
}

int main() {
    try {
        for (const auto symbols : {1U, 8U, 32U}) {
            for (const auto seed : {0ULL, 42ULL, 20261018ULL}) compare_workload(seed, symbols, 2000);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
