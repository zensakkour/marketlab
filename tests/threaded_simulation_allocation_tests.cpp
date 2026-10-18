#include "partitioned_test_support.hpp"
#include "../examples/partitioned_market_scenario.hpp"

#include <marketlab/threaded_simulation.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <new>
#include <string>
#include <thread>

namespace {
const auto coordinator = std::this_thread::get_id();
std::ptrdiff_t coordinator_budget = -1;
std::atomic<std::ptrdiff_t> worker_budget{-1};
thread_local std::ptrdiff_t local_worker_budget = -2;
}

// Keep replacement boundaries intact for GCC allocation-pair diagnostics.
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void* operator new(std::size_t bytes) {
    auto* budget = &coordinator_budget;
    if (std::this_thread::get_id() != coordinator) {
        if (local_worker_budget == -2) local_worker_budget = worker_budget.load();
        budget = &local_worker_budget;
    }
    if (*budget >= 0 && (*budget)-- == 0) throw std::bad_alloc{};
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

using namespace multi_test;

void prepare(ThreadedSimulation& runner) {
    for (const auto& request : std::array<SimulationRequest, 8>{
        order(1, 1, 1, Side::Sell, 100, 3, 0), order(1, 2, 1, Side::Sell, 101, 5, 1),
        order(1, 3, 2, Side::Buy, 101, 10, 2), cancel(1, 3, 2, 3),
        order(1, 4, 2, Side::Buy, 100, 0, 4), order(2, 1, 2, Side::Buy, 200, 9, 7),
        order(2, 2, 1, Side::Sell, 200, 4, 8), cancel(2, 1, 2, 9)}) {
        require(runner.schedule(request).has_value(), "thread fault fixture scheduling failed");
    }
}

std::map<std::size_t, std::string> records(const std::vector<WorkerBatch>& batches) {
    std::map<std::size_t, std::string> result;
    for (const auto& batch : batches) {
        require(batch.succeeded(), "fault-free batch failed");
        for (const auto& step : batch.steps) result[batch.worker_index] += event_text(step.events);
    }
    return result;
}

void test_constructor_failure() {
    auto configs = configurations();
    configs[0].symbol.assign(200, 'B');
    configs[1].symbol.assign(200, 'A');
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 3, worker_bindings(registry, 3)};
    const auto* borrowed = registry.find({1});
    const auto* bindings = plan.assignments().data();
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        bool failed = false;
        coordinator_budget = fault;
        try {
            ThreadedSimulation runner{registry, plan, participants};
            coordinator_budget = -1;
            require(runner.find({1})->instrument().symbol == configs[1].symbol && runner.run().empty(),
                "threaded constructor changed configuration");
        } catch (const std::bad_alloc&) {
            coordinator_budget = -1;
            failed = true;
            ++failures;
        }
        require(registry.find({1}) == borrowed && plan.assignments().data() == bindings,
            "failed threaded construction changed source views");
        ThreadedSimulation retry{registry, plan, participants};
        require(retry.find({1})->book().empty() && retry.queue(0)->last_sequence().value == 0,
            "constructor retry retained partial state");
        if (!failed) break;
    }
    require(failures != 0, "threaded constructor allocations not exercised");
}

void test_preparation_and_launch_failure() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 2, worker_bindings(registry, 2)};
    ThreadedSimulation baseline{registry, plan, participants};
    prepare(baseline);
    const auto expected = records(baseline.run());
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        ThreadedSimulation runner{registry, plan, participants};
        prepare(runner);
        const auto before_a = state(*runner.find({1})), before_b = state(*runner.find({2}));
        const auto* head_a = runner.queue(0)->next();
        const auto* head_b = runner.queue(1)->next();
        bool failed = false;
        coordinator_budget = fault;
        try {
            const auto result = runner.run();
            coordinator_budget = -1;
            require(records(result) == expected, "successful launch publication differs");
        } catch (const std::bad_alloc&) {
            coordinator_budget = -1;
            failed = true;
            ++failures;
        }
        if (failed) {
            require(state(*runner.find({1})) == before_a && state(*runner.find({2})) == before_b &&
                runner.queue(0)->next() == head_a && runner.queue(1)->next() == head_b &&
                runner.queue(0)->size() == 5 && runner.queue(1)->size() == 3,
                "preparation/launch failure allowed processing or lost pending arrivals");
            require(records(runner.run()) == expected, "launch failure retry differs");
        }
        require(state(*runner.find({1})) == state(*baseline.find({1})) &&
            state(*runner.find({2})) == state(*baseline.find({2})), "launch retry final state differs");
        if (!failed) break;
    }
    require(failures >= 4, "output preparation and thread launch allocation failures not exercised");
}

void test_worker_failure_and_prefix_retry() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 2, worker_bindings(registry, 2)};
    ThreadedSimulation baseline{registry, plan, participants};
    prepare(baseline);
    const auto expected = records(baseline.run());
    bool saw_prefix = false, saw_independent_completion = false;
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        ThreadedSimulation runner{registry, plan, participants};
        prepare(runner);
        worker_budget.store(fault);
        const auto batches = runner.run();
        worker_budget.store(-1);
        bool failed = false, completed = false;
        std::map<std::size_t, std::string> actual;
        for (const auto& batch : batches) {
            require(!batch.error, "allocation fault became engine diagnostic");
            if (batch.exception) {
                bool allocation_error = false;
                try { std::rethrow_exception(batch.exception); }
                catch (const std::bad_alloc&) { allocation_error = true; }
                require(allocation_error, "worker exception identity was lost");
                failed = true;
                ++failures;
                saw_prefix = saw_prefix || !batch.steps.empty();
            } else completed = true;
            const auto id = InstrumentId{batch.worker_index + 1};
            Exchange partial{*registry.find(id), participants};
            for (const auto& step : batch.steps) {
                const auto replayed = isolated_process(partial, step.arrival.request);
                require(replayed && event_text(*replayed) == event_text(step.events), "worker prefix differs from replay");
                actual[batch.worker_index] += event_text(step.events);
            }
            require(state(*runner.find(id)) == state(partial), "unpublished commit or partial failing request observed");
            const auto total = batch.worker_index == 0 ? 5U : 3U;
            const auto* queue = runner.queue(batch.worker_index);
            require(queue->size() == total - batch.steps.size() && queue->last_sequence().value == total,
                "worker failure changed arrival count or insertion counter");
            if (batch.exception) {
                require(queue->next() && queue->next()->sequence.value == batch.steps.size() + 1,
                    "failed arrival was removed from its queue");
            } else require(queue->empty(), "successful worker stopped before draining");
        }
        saw_independent_completion = saw_independent_completion || (failed && completed);
        for (const auto& batch : runner.run()) {
            require(batch.succeeded(), "worker allocation retry failed");
            for (const auto& step : batch.steps) actual[batch.worker_index] += event_text(step.events);
        }
        require(actual == expected && state(*runner.find({1})) == state(*baseline.find({1})) &&
            state(*runner.find({2})) == state(*baseline.find({2})), "worker prefix+retry duplicated or lost committed work");
        if (!failed) break;
    }
    require(failures != 0 && saw_prefix && saw_independent_completion,
        "worker faults did not cover retained prefixes and independent completion");
}

void test_example_worker_failure() {
    worker_budget.store(0);
    bool failed = false;
    try { static_cast<void>(partitioned_example::run(2, partitioned_example::ExecutionMode::Threaded)); }
    catch (const std::bad_alloc&) { failed = true; }
    worker_budget.store(-1);
    require(failed, "finite example hid a worker allocation failure");
    const auto fresh = partitioned_example::run(2, partitioned_example::ExecutionMode::Threaded);
    require(fresh.events.size() == 25 && fresh.worker_steps == std::vector<std::size_t>{7, 4},
        "fresh example after worker failure retained partial state or unjoined threads");
}

void test_empty_run_without_allocation() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 4, worker_bindings(registry, 4)};
    ThreadedSimulation runner{registry, plan, participants};
    coordinator_budget = 0;
    const auto empty = runner.run();
    const auto missing = runner.schedule(cancel(99, 1, 1, 0));
    const auto* found = runner.find({1});
    const auto* unused = runner.queue(3);
    coordinator_budget = -1;
    require(empty.empty() && found && !unused && missing.error() == PartitionedScheduleError::UnknownInstrument,
        "empty/read/refused threaded path allocated or changed behavior");
}

int main() {
    try {
        test_constructor_failure();
        test_preparation_and_launch_failure();
        test_worker_failure_and_prefix_retry();
        test_empty_run_without_allocation();
        test_example_worker_failure();
    } catch (const std::exception& error) {
        coordinator_budget = -1;
        worker_budget.store(-1);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
