#include "partitioned_test_support.hpp"

#include <marketlab/participant_pnl.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace multi_test;

static_assert(!std::is_copy_constructible_v<PartitionedSimulation>);
static_assert(!std::is_move_constructible_v<PartitionedSimulation>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedSimulation&>().find({1})), const Exchange*>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedSimulation&>().queue(0)), const SimulationQueue*>);
static_assert(noexcept(std::declval<const PartitionedSimulation&>().find({1})));
static_assert(noexcept(std::declval<const PartitionedSimulation&>().queue(0)));
static_assert(detail::partitioned_step_error(MultiInstrumentError::RequestSequenceExhausted) ==
    PartitionedStepError::RequestSequenceExhausted);
static_assert(detail::partitioned_step_error(MultiInstrumentError::EventSequenceExhausted) ==
    PartitionedStepError::EventSequenceExhausted);
static_assert(detail::partitioned_step_error(MultiInstrumentError::OutputCapacityExceeded) ==
    PartitionedStepError::OutputCapacityExceeded);

void test_lifecycle_and_local_clocks() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    const std::array bindings{InstrumentWorkerAssignment{{1}, 0}, InstrumentWorkerAssignment{{2}, 0}};
    WorkerAssignmentPlan plan{registry, 3, bindings};
    PartitionedSimulation runner{registry, plan, participants};
    const auto* queue = runner.queue(0);
    const auto* a = runner.find({1});
    require(queue && !runner.queue(1) && !runner.queue(3) && !runner.find({99}), "partition lookup differs");
    require(runner.step(0)->has_value() == false && runner.step(2)->has_value() == false &&
        runner.step(3).error() == PartitionedStepError::InvalidWorker, "empty/invalid worker step differs");
    require(runner.schedule(order(1, 1, 1, Side::Buy, 100, 5, 100))->value == 1 &&
        runner.schedule(order(2, 1, 1, Side::Sell, 200, 5, 1))->value == 2,
        "worker sequence did not follow insertion");
    const auto first = runner.step(0);
    require(first && *first && (*first)->arrival.sequence.value == 2 && (*first)->worker_index == 0,
        "worker queue ignored arrival time");
    require(runner.step(0)->has_value() && a->current_time() == LogicalTime{100} &&
        runner.find({2})->current_time() == LogicalTime{1}, "instrument clocks were merged");
    const auto before = state(*a);
    require(runner.schedule(order(1, 2, 1, Side::Buy, 100, 1, 99)).error() ==
        PartitionedScheduleError::InvalidTimestamp && runner.schedule(cancel(99, 1, 1, 0)).error() ==
        PartitionedScheduleError::UnknownInstrument && queue->last_sequence().value == 2 && queue->empty() &&
        state(*a) == before, "refused scheduling changed sequence, queue or book");
    require(runner.schedule(cancel(2, 1, 1, 2))->value == 3 &&
        runner.schedule(cancel(1, 1, 1, 100))->value == 4,
        "current-time/resumed other-instrument scheduling refused");
    const auto resumed = runner.step(0);
    require(resumed && *resumed && (*resumed)->arrival.sequence.value == 3 &&
        std::holds_alternative<OrderCancelled>((*resumed)->events.front()), "drained worker did not resume");
    require(runner.step(0)->has_value() && runner.step(0)->has_value() == false &&
        a == runner.find({1}) && queue == runner.queue(0), "step invalidated exchange/queue views");
    auto malformed = order(2, 2, 99, Side::Buy, -1, 0, 3);
    require(runner.schedule(malformed)->value == 5, "raw invalid fields refused before exchange");
    const auto rejected = runner.step(0);
    require(rejected && *rejected && (*rejected)->events.size() == 1 &&
        std::holds_alternative<OrderRejected>((*rejected)->events.front()) && queue->empty() &&
        runner.find({2})->current_time() == LogicalTime{3}, "exchange rejection did not consume arrival");
}

void test_configuration_and_source_lifetime() {
    std::optional<PartitionedSimulation> owned;
    {
        auto configs = configurations();
        InstrumentRegistry registry{configs};
        auto bindings = worker_bindings(registry, 2);
        WorkerAssignmentPlan plan{registry, 2, bindings};
        auto ids = participants;
        owned.emplace(registry, plan, ids);
        configs[0].symbol = "CHANGED";
        bindings[0].worker_index = 99;
        ids.fill(ParticipantId{99});
    }
    require(owned->find({1})->instrument().symbol == "ALPHA" && owned->plan().worker_for({2}) == 1 &&
        owned->schedule(order(1, 1, 1, Side::Buy, 100, 1, 0)).has_value(), "runner borrowed caller configuration");
    const auto result = owned->step(0);
    require(result && *result && std::holds_alternative<OrderAccepted>((*result)->events.front()),
        "fixed participant configuration changed");
    const auto configs = configurations();
    InstrumentRegistry registry{configs}, empty;
    WorkerAssignmentPlan plan{registry, 2, worker_bindings(registry, 2)};
    bool refused = false;
    try { PartitionedSimulation invalid{empty, plan, participants}; }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "registry/assignment mismatch accepted");
    auto foreign_configs = configs;
    foreign_configs[0].id = {3};
    InstrumentRegistry foreign{foreign_configs};
    refused = false;
    try { PartitionedSimulation invalid{foreign, plan, participants}; }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "same-size registry with different membership accepted");
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const std::array sparse_bindings{InstrumentWorkerAssignment{{1}, maximum - 1},
        InstrumentWorkerAssignment{{2}, maximum - 2}};
    WorkerAssignmentPlan sparse_plan{registry, maximum, sparse_bindings};
    PartitionedSimulation sparse{registry, sparse_plan, participants};
    require(sparse.schedule(cancel(1, 1, 1, 0))->value == 1 &&
        sparse.step(maximum - 1)->has_value() && !sparse.queue(0), "large sparse active worker indexes differ");
    WorkerAssignmentPlan empty_plan{empty, std::numeric_limits<std::size_t>::max(), {}};
    PartitionedSimulation idle{empty, empty_plan, participants};
    require(idle.step(std::numeric_limits<std::size_t>::max() - 1)->has_value() == false &&
        !idle.find({1}) && idle.schedule(cancel(1, 1, 1, 0)).error() == PartitionedScheduleError::UnknownInstrument,
        "sparse empty plan created workers or accepted unknown instrument");
    for (const auto* config_registry : {&registry, &empty}) {
        const auto& selected_plan = config_registry == &registry ? plan : empty_plan;
        const std::array invalid_ids{ParticipantId{0}};
        refused = false;
        try { PartitionedSimulation invalid{*config_registry, selected_plan, invalid_ids}; }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused, "empty worker set bypassed participant validation");
    }
}

void compare_partitioning(std::uint64_t seed, std::size_t symbols, std::size_t workers, bool reverse) {
    const auto configs = workload_instruments(symbols);
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, workers, worker_bindings(registry, workers)};
    PartitionedSimulation runner{registry, plan, participants};
    std::map<InstrumentId, Exchange> baseline;
    std::map<InstrumentId, ParticipantLedger> accounts, expected_accounts;
    for (const auto& config : configs) {
        baseline.try_emplace(config.id, config, participants);
        accounts.try_emplace(config.id, config.id, participants);
        expected_accounts.try_emplace(config.id, config.id, participants);
    }
    const auto script = instrument_workload(seed, symbols, 1200);
    std::map<std::size_t, std::vector<SimulationEvent>> ordered;
    for (const auto& request : script) {
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const auto owner = *plan.worker_for(id);
        auto& arrivals = ordered[owner];
        const auto sequence = runner.schedule(request);
        require(sequence && sequence->value == arrivals.size() + 1, "wrong destination queue/insertion sequence");
        arrivals.push_back({*sequence, request});
    }
    for (auto& [index, arrivals] : ordered) {
        static_cast<void>(index);
        std::ranges::sort(arrivals, scheduled_before);
    }
    // Independent per-instrument replay follows input order: generated local times
    // never decrease, so queue repartitioning cannot change that instrument's order.
    std::map<InstrumentId, std::string> expected_events, actual_events;
    for (const auto& request : script) {
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const auto events = isolated_process(baseline.at(id), request);
        require(events.has_value(), "isolated workload failed");
        expected_events[id] += event_text(*events);
        require(expected_accounts.at(id).apply(executions_in(*events)).has_value(), "isolated ledger failed");
    }
    std::map<std::size_t, std::size_t> cursors;
    std::size_t processed = 0;
    while (processed < script.size()) {
        for (std::size_t turn = 0; turn < workers; ++turn) {
            const auto owner = reverse ? workers - 1 - turn : turn;
            const auto result = runner.step(owner);
            require(result.has_value(), "partitioned workload failed");
            if (!*result) continue;
            const auto& step = **result;
            const auto& expected_arrival = ordered.at(owner).at(cursors[owner]++);
            const auto id = std::visit([](const auto& value) { return value.instrument_id; }, step.arrival.request);
            require(step.worker_index == owner && step.arrival.sequence == expected_arrival.sequence &&
                scheduled_time(step.arrival) == scheduled_time(expected_arrival) && plan.worker_for(id) == owner,
                "worker processed wrong arrival or queue order");
            actual_events[id] += event_text(step.events);
            require(accounts.at(id).apply(executions_in(step.events)).has_value(), "partitioned ledger failed");
            ++processed;
        }
    }
    require(actual_events == expected_events, "partitioning changed an instrument's event stream");
    const auto mark = Price::from_ticks(100).value();
    for (const auto& config : configs) {
        require(state(*runner.find(config.id), 64) == state(baseline.at(config.id), 64) &&
            runner.find(config.id)->book().check_invariants(), "partitioning changed final book/clock/market data");
        for (const auto participant : participants) {
            require(accounts.at(config.id).account(participant) == expected_accounts.at(config.id).account(participant) &&
                fifo_pnl(accounts.at(config.id), participant, mark) ==
                    fifo_pnl(expected_accounts.at(config.id), participant, mark), "partitioned cash/positions/PnL differ");
        }
    }
    for (std::size_t index = 0; index < workers; ++index) {
        require(runner.step(index)->has_value() == false, "drained partition retained an arrival");
    }
}

int main() {
    try {
        test_lifecycle_and_local_clocks();
        test_configuration_and_source_lifetime();
        for (const auto seed : {0ULL, 42ULL}) {
            for (const auto symbols : {1U, 24U}) {
                for (const auto workers : {1U, 4U, 7U}) {
                    compare_partitioning(seed, symbols, workers, false);
                    compare_partitioning(seed, symbols, workers, true);
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
