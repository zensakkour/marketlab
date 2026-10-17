#include <marketlab/participant_fills.hpp>
#include <marketlab/simulation.hpp>

#include "participant_ledger_test_support.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <utility>
#include <vector>

using namespace ledger_test;

static_assert(noexcept(participant_fills(
    std::declval<const ParticipantLedger&>(), ParticipantId{1})));

void test_counts_and_owned_results() {
    ParticipantLedger ledger{{1}, participants};
    require(participant_fills(ledger, {1}) == ParticipantFills{} &&
        !participant_fills(ledger, {0}) && !participant_fills(ledger, {99}),
        "empty or unknown participant fill counts differ");
    auto selling = trade(3, 2, 2, 4, 2, 1);
    selling.aggressive_side = Side::Sell;
    auto self_selling = trade(5, 4, 4, 7, 1, 1);
    self_selling.aggressive_side = Side::Sell;
    const std::array executions{
        trade(1, 1, 1, 3), trade(2, 1, 1, 2, 1, 3), selling,
        trade(4, 3, 3, 5, 1, 1), self_selling};
    require(ledger.apply(executions).has_value(), "fill fixture refused");
    const auto first = participant_fills(ledger, {1});
    require(first == ParticipantFills{4, 3, 5, 2} &&
        participant_fills(ledger, {2}) == ParticipantFills{1, 1, 0, 2} &&
        participant_fills(ledger, {3}) == ParticipantFills{0, 1, 0, 1},
        "direction, role or self-match fill counts differ");

    ParticipantLedger replay{{1}, participants};
    for (const auto& execution : executions) {
        require(replay.apply(std::span{&execution, 1}).has_value(), "incremental fill replay refused");
    }
    for (const auto id : participants) {
        require(participant_fills(ledger, id) == participant_fills(replay, id), "fill history replay differs");
    }

    const auto* borrowed = ledger.history().data();
    const auto duplicate = ledger.apply(std::span{executions}.last(1));
    require(!duplicate && duplicate.error() == AccountingError::OutOfOrder &&
        participant_fills(ledger, {1}) == first && ledger.history().data() == borrowed,
        "refused execution changed fill counts or history");
    const auto last = trade(6, 5, 5, INT64_MAX, 3, 1);
    require(ledger.apply(std::span{&last, 1}).has_value(), "large-size fill refused");
    require(first == ParticipantFills{4, 3, 5, 2} &&
        participant_fills(ledger, {1}) == ParticipantFills{4, 4, 5, 3},
        "owned fill result changed or large quantity counted as multiple fills");
}

void test_scheduled_partial_and_multilevel_fills() {
    Simulation simulation{Instrument{{1}, "FILLS", TickSize::from_decimal(1, 2).value()}, participants};
    ParticipantLedger ledger{{1}, participants};
    const std::array<SimulationRequest, 6> requests{
        OrderRequest{{0}, {1}, {2}, {1}, Side::Sell, OrderType::Limit, 3, 101},
        OrderRequest{{0}, {1}, {3}, {2}, Side::Sell, OrderType::Limit, 5, 102},
        OrderRequest{{1}, {1}, {1}, {3}, Side::Buy, OrderType::Market, 1, std::nullopt},
        OrderRequest{{2}, {1}, {1}, {4}, Side::Buy, OrderType::Market, 10, std::nullopt},
        CancelRequest{{3}, {1}, {2}, {1}},
        OrderRequest{{4}, {1}, {1}, {5}, Side::Buy, OrderType::Limit, 0, 100}};
    for (const auto& request : requests) require(simulation.schedule(request).has_value(), "fill scheduling failed");
    for (;;) {
        const auto step = simulation.step();
        require(step.has_value(), "fill delivery failed");
        if (!*step) break;
        std::vector<Execution> executions;
        for (const auto& event : step->value().events) {
            if (const auto* execution = std::get_if<Execution>(&event)) executions.push_back(*execution);
        }
        require(ledger.apply(executions).has_value(), "fill accounting failed");
    }
    require(ledger.history().size() == 3 &&
        participant_fills(ledger, {1}) == ParticipantFills{3, 0, 3, 0} &&
        participant_fills(ledger, {2}) == ParticipantFills{0, 2, 0, 2} &&
        participant_fills(ledger, {3}) == ParticipantFills{0, 1, 0, 1},
        "partial/multilevel fills, expiry or rejections changed execution-only counts");
}

int main() {
    try {
        test_counts_and_owned_results();
        test_scheduled_partial_and_multilevel_fills();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
