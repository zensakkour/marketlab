#include "partitioned_test_support.hpp"
#include "../examples/market_making_scenario.hpp"

#include <marketlab/participant_pnl.hpp>
#include <marketlab/streaming_simulation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace multi_test;

static_assert(!std::is_copy_constructible_v<StreamingSimulation>);
static_assert(!std::is_move_constructible_v<StreamingSimulation>);
static_assert(std::is_same_v<decltype(std::declval<const StreamingSimulation&>().find({1})), const Exchange*>);
static_assert(std::is_same_v<decltype(std::declval<const StreamingSimulation&>().queue(0)), const BoundedIngressQueue*>);
static_assert(noexcept(std::declval<const StreamingSimulation&>().plan()));
static_assert(noexcept(std::declval<const StreamingSimulation&>().is_closed()));
static_assert(detail::streaming_schedule_error(IngressPushError::SequenceExhausted) == StreamingScheduleError::SequenceExhausted);
static_assert(detail::streaming_step_error(MultiInstrumentError::RequestSequenceExhausted) == StreamingStepError::RequestSequenceExhausted);
static_assert(detail::streaming_step_error(MultiInstrumentError::EventSequenceExhausted) == StreamingStepError::EventSequenceExhausted);
static_assert(detail::streaming_step_error(MultiInstrumentError::OutputCapacityExceeded) == StreamingStepError::OutputCapacityExceeded);

void test_routing_close_and_owned_results() {
    std::optional<StreamingSimulation> owner;
    {
        auto configs = configurations();
        InstrumentRegistry registry{configs};
        WorkerAssignmentPlan plan{registry, 3, worker_bindings(registry, 3)};
        auto ids = participants;
        owner.emplace(registry, plan, ids, 1);
        configs[1].symbol = "CHANGED";
        ids.fill(ParticipantId{99});
    }
    require(owner->find({1})->instrument().symbol == "ALPHA" && !owner->find({99}) && !owner->queue(2) &&
        owner->step(2).error() == StreamingStepError::Empty && owner->step(3).error() == StreamingStepError::InvalidWorker,
        "streaming ownership, idle or invalid worker behavior differs");
    const auto* borrowed = owner->find({1});
    const auto* queue = owner->queue(0);
    require(owner->try_schedule(order(1, 1, 1, Side::Buy, 100, 3, 10))->value == 1 &&
        owner->try_schedule(cancel(1, 1, 1, 100)).error() == StreamingScheduleError::Full &&
        owner->try_schedule(cancel(1, 1, 1, 9)).error() == StreamingScheduleError::InvalidTimestamp &&
        owner->try_schedule(cancel(99, 1, 1, 0)).error() == StreamingScheduleError::UnknownInstrument,
        "routing/full refusal consumed state or changed diagnostics");
    require(owner->try_schedule(order(2, 1, 2, Side::Sell, 200, 4, 1))->value == 1,
        "independent partitions share accepted-time watermark or sequences");
    const auto first = owner->step(0);
    require(first && first->arrival.sequence.value == 1 && owner->try_schedule(cancel(1, 1, 1, 10))->value == 2,
        "successful step did not free ingress capacity or refusal advanced time");
    const auto cancelled = owner->step(0);
    require(cancelled && std::holds_alternative<OrderCancelled>(cancelled->events[0]) &&
        owner->step(0).error() == StreamingStepError::Empty, "committed cancel was not consumed");
    owner->close();
    owner->close();
    require(owner->is_closed() && owner->queue(1)->size() == 1 && owner->queue(1)->is_closed() &&
        owner->try_schedule(cancel(1, 1, 1, 10)).error() == StreamingScheduleError::Closed &&
        owner->try_schedule(cancel(99, 1, 1, 0)).error() == StreamingScheduleError::UnknownInstrument &&
        owner->step(0).error() == StreamingStepError::Closed && owner->step(2).error() == StreamingStepError::Closed,
        "close discarded work, reopened ingress or masked routing diagnostics");
    const auto other = owner->step(1);
    require(other && other->arrival.sequence.value == 1 && owner->step(1).error() == StreamingStepError::Closed &&
        owner->find({1}) == borrowed && owner->queue(0) == queue, "close/drain changed owned view addresses");
    const auto text = event_text(first->events);
    owner.reset();
    require(!text.empty() && event_text(first->events) == text && scheduled_time(first->arrival) == LogicalTime{10},
        "streaming result did not survive owner destruction");
}

void test_configuration_and_partition_time() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs}, empty;
    WorkerAssignmentPlan shared{registry, 1, worker_bindings(registry, 1)}, idle{empty, 1, {}};
    StreamingSimulation runner{registry, shared, participants, 2};
    require(runner.try_schedule(order(1, 1, 1, Side::Buy, 100, 2, 100))->value == 1 &&
        runner.try_schedule(order(2, 1, 2, Side::Sell, 200, 2, 2)).error() == StreamingScheduleError::InvalidTimestamp &&
        runner.try_schedule(order(2, 1, 2, Side::Sell, 200, 2, 100))->value == 2,
        "FIFO accepted decreasing partition time despite independent instrument clocks");
    require(runner.step(0).has_value() && runner.step(0).has_value() &&
        runner.find({1})->book().find({1}) && runner.find({2})->book().find({1}), "streaming instrument IDs are not isolated");
    require(runner.try_schedule(order(1, 2, 99, Side::Buy, 100, 0, 100))->value == 3,
        "producer performed exchange business validation");
    const auto rejected = runner.step(0);
    require(rejected && std::holds_alternative<OrderRejected>(rejected->events[0]) &&
        runner.step(0).error() == StreamingStepError::Empty, "exchange rejection was not a committed streaming step");
    for (const auto* source : {&registry, &empty}) {
        const auto& plan = source == &registry ? shared : idle;
        bool refused_capacity = false, refused_participant = false;
        try { StreamingSimulation invalid{*source, plan, participants, 0}; }
        catch (const std::invalid_argument&) { refused_capacity = true; }
        const std::array invalid_ids{ParticipantId{0}};
        try { StreamingSimulation invalid{*source, plan, invalid_ids, 1}; }
        catch (const std::invalid_argument&) { refused_participant = true; }
        require(refused_capacity && refused_participant, "empty registry bypassed streaming configuration validation");
    }
    bool mismatch = false;
    try { StreamingSimulation invalid{empty, shared, participants, 1}; }
    catch (const std::invalid_argument&) { mismatch = true; }
    require(mismatch, "streaming owner accepted a mismatched registry/plan");
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const std::array bindings{InstrumentWorkerAssignment{{1}, maximum - 1}, InstrumentWorkerAssignment{{2}, maximum - 2}};
    WorkerAssignmentPlan sparse_plan{registry, maximum, bindings}, empty_plan{empty, maximum, {}};
    StreamingSimulation sparse{registry, sparse_plan, participants, 1}, no_instruments{empty, empty_plan, participants, 1};
    require(!sparse.queue(0) && no_instruments.step(maximum - 1).error() == StreamingStepError::Empty &&
        sparse.try_schedule(cancel(1, 1, 1, 0))->value == 1, "streaming storage scales with idle worker indexes");
    sparse.close();
    no_instruments.close();
    require(sparse.step(maximum - 1).has_value() && sparse.step(maximum - 1).error() == StreamingStepError::Closed &&
        no_instruments.step(maximum - 1).error() == StreamingStepError::Closed, "sparse close/drain failed");
}

struct Output {
    std::vector<WorkerStep> steps;
    std::exception_ptr failure;
};

void compare_concurrent_delivery(std::uint64_t seed, std::size_t symbols, std::size_t workers, std::size_t capacity) {
    const auto configs = workload_instruments(symbols);
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, workers, worker_bindings(registry, workers)};
    StreamingSimulation runner{registry, plan, participants, capacity};
    PartitionedSimulation reference{registry, plan, participants};
    auto script = instrument_workload(seed, symbols, 1200);
    for (const auto& request : script) require(reference.schedule(request).has_value(), "reference scheduling failed");
    std::stable_sort(script.begin(), script.end(), [](const auto& left, const auto& right) {
        const auto time = [](const auto& request) { return std::visit([](const auto& value) { return value.timestamp; }, request); };
        return time(left) < time(right);
    });
    std::map<std::size_t, Output> outputs;
    std::map<std::size_t, std::size_t> submitted;
    for (const auto& binding : plan.assignments()) {
        outputs.try_emplace(binding.worker_index);
        submitted.try_emplace(binding.worker_index, 0);
    }
    for (auto& [index, output] : outputs) { static_cast<void>(index); output.steps.reserve(script.size()); }
    std::atomic<bool> failed{false};
    std::vector<std::jthread> threads;
    threads.reserve(outputs.size());
    for (auto& [index, output] : outputs) {
        threads.emplace_back([&, index, result = &output](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    auto step = runner.step(index);
                    if (!step) {
                        if (step.error() == StreamingStepError::Closed) return;
                        require(step.error() == StreamingStepError::Empty, "streaming worker engine failed");
                        std::this_thread::yield();
                        continue;
                    }
                    result->steps.push_back(std::move(*step));
                }
            } catch (...) { result->failure = std::current_exception(); failed.store(true); }
        });
    }
    for (const auto& request : script) {
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const auto index = *plan.worker_for(id);
        while (!failed.load()) {
            const auto accepted = runner.try_schedule(request);
            if (accepted) {
                require(accepted->value == ++submitted.at(index), "streaming sequence changed with backpressure");
                break;
            }
            require(accepted.error() == StreamingScheduleError::Full, "ordered streaming input was refused");
            std::this_thread::yield();
        }
        if (failed.load()) break;
    }
    runner.close();
    for (auto& thread : threads) thread.join();
    for (const auto& [index, output] : outputs) { static_cast<void>(index); if (output.failure) std::rethrow_exception(output.failure); }
    std::map<InstrumentId, ParticipantLedger> accounts, expected_accounts;
    for (const auto& config : configs) {
        accounts.try_emplace(config.id, config.id, participants);
        expected_accounts.try_emplace(config.id, config.id, participants);
    }
    std::size_t total = 0;
    for (const auto& [index, output] : outputs) {
        require(output.steps.size() == submitted.at(index) && runner.queue(index)->size() == 0 &&
            runner.step(index).error() == StreamingStepError::Closed, "streaming close did not drain accepted work");
        for (std::size_t position = 0; position < output.steps.size(); ++position) {
            const auto& step = output.steps[position];
            const auto expected = reference.step(index);
            require(expected && *expected && step.worker_index == index && step.arrival.sequence.value == position + 1 &&
                scheduled_time(step.arrival) == scheduled_time((**expected).arrival) &&
                event_text(step.events) == event_text((**expected).events), "streaming changed FIFO delivery or events");
            const auto id = std::visit([](const auto& value) { return value.instrument_id; }, step.arrival.request);
            require(accounts.at(id).apply(executions_in(step.events)).has_value() &&
                expected_accounts.at(id).apply(executions_in((**expected).events)).has_value(), "streaming accounting refused");
            ++total;
        }
        const auto drained = reference.step(index);
        require(drained && !*drained, "streaming omitted reference arrivals");
    }
    require(total == script.size(), "streaming lost or duplicated submitted work");
    const auto mark = Price::from_ticks(100).value();
    for (const auto& config : configs) {
        require(state(*runner.find(config.id), 64) == state(*reference.find(config.id), 64) &&
            runner.find(config.id)->book().check_invariants(), "streaming final book/market state differs");
        for (const auto participant : participants) {
            require(accounts.at(config.id).account(participant) == expected_accounts.at(config.id).account(participant) &&
                fifo_pnl(accounts.at(config.id), participant, mark) == fifo_pnl(expected_accounts.at(config.id), participant, mark),
                "streaming cash/position/FIFO PnL differs");
        }
    }
}

void test_reference_delivery() {
    constexpr std::array owners{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}, ParticipantId{4}};
    for (const auto seed : {0ULL, 42ULL}) {
        const auto reference = market_making_example::run(seed);
        const std::array configs{Instrument{{1}, "REFERENCE", TickSize::from_decimal(1, 2).value()}};
        InstrumentRegistry registry{configs};
        const std::array binding{InstrumentWorkerAssignment{{1}, 0}};
        WorkerAssignmentPlan plan{registry, 1, binding};
        StreamingSimulation runner{registry, plan, owners, 1};
        Output output;
        output.steps.reserve(reference.arrivals.size());
        std::atomic<bool> failed{false};
        std::jthread consumer{[&](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    auto step = runner.step(0);
                    if (step) output.steps.push_back(std::move(*step));
                    else if (step.error() == StreamingStepError::Closed) return;
                    else {
                        require(step.error() == StreamingStepError::Empty, "streaming reference engine failed");
                        std::this_thread::yield();
                    }
                }
            } catch (...) { output.failure = std::current_exception(); failed.store(true); }
        }};
        // Feed only the reference's delivered arrivals, excluding its t=500 boundary.
        for (const auto& arrival : reference.arrivals) {
            while (!failed.load()) {
                const auto accepted = runner.try_schedule(arrival.request);
                if (accepted) break;
                require(accepted.error() == StreamingScheduleError::Full, "streaming reference input was refused");
                std::this_thread::yield();
            }
            if (failed.load()) break;
        }
        runner.close();
        consumer.join();
        if (output.failure) std::rethrow_exception(output.failure);
        require(output.steps.size() == reference.arrivals.size(), "streaming reference lost an arrival");
        Exchange replay{configs[0], owners};
        ParticipantLedger ledger{{1}, owners};
        TimeWeightedSpread spread;
        std::string events;
        for (const auto& step : output.steps) {
            const auto expected = isolated_process(replay, step.arrival.request);
            require(expected && event_text(*expected) == event_text(step.events) &&
                ledger.apply(executions_in(step.events)).has_value() &&
                spread.observe(scheduled_time(step.arrival), best_prices(replay).spread_ticks()).has_value(),
                "streaming reference replay/accounting/spread differs");
            events += event_text(step.events);
        }
        require(events == event_text(reference.events) && state(*runner.find({1}), 10000) == state(replay, 10000) &&
            spread.snapshot(reference.horizon).value() == reference.spread, "streaming changed reference events, state or spread");
        for (const auto& account : reference.participants) {
            require(ledger.account(account.id) == account.account && fifo_pnl(ledger, account.id, reference.mark) == account.pnl &&
                participant_fills(ledger, account.id) == account.fills, "streaming reference participant report differs");
        }
    }
}

int main() {
    try {
        test_routing_close_and_owned_results();
        test_configuration_and_partition_time();
        test_reference_delivery();
        for (const auto seed : {0ULL, 42ULL}) {
            for (const auto symbols : {1U, 12U}) {
                for (const auto workers : {1U, 4U, 7U}) {
                    for (const auto capacity : {1U, 7U, 64U}) {
                        try { compare_concurrent_delivery(seed, symbols, workers, capacity); }
                        catch (...) {
                            std::cerr << "seed=" << seed << " symbols=" << symbols << " workers=" << workers << " capacity=" << capacity << '\n';
                            throw;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
