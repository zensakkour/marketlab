#include "partitioned_test_support.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <variant>

namespace {
std::ptrdiff_t allocations_before_failure = -1;
}

// Keep both replacement boundaries intact for GCC allocation-pair diagnostics.
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) throw std::bad_alloc{};
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

SimulationRequest pending_request(int scenario) {
    if (scenario == 3) return cancel(1, 1, 1, 2);
    if (scenario == 5) return cancel(1, 1, 2, 2);
    auto request = order(1, 3, 2, Side::Buy, scenario == 0 ? 99 : 101, 10, 2);
    if (scenario == 2) {
        request.type = OrderType::Market;
        request.limit_price_ticks.reset();
    }
    if (scenario == 4) request.quantity_units = 0;
    return request;
}

void prepare(PartitionedSimulation& runner) {
    require(runner.schedule(order(1, 1, 1, Side::Sell, 100, 3, 1)).has_value() &&
        runner.schedule(order(1, 2, 1, Side::Sell, 101, 5, 1)).has_value() &&
        runner.schedule(order(2, 1, 2, Side::Buy, 200, 9, 7)).has_value(), "fault fixture scheduling failed");
    require(runner.step(0)->has_value() && runner.step(0)->has_value() && runner.step(1)->has_value(),
        "fault fixture processing failed");
}

void test_constructor_failure() {
    auto configs = configurations();
    configs[0].symbol.assign(200, 'B');
    configs[1].symbol.assign(200, 'A');
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 3, worker_bindings(registry, 3)};
    const auto* borrowed = registry.find({1});
    const auto* assignment = plan.assignments().data();
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        bool failed = false;
        allocations_before_failure = fault;
        try {
            PartitionedSimulation runner{registry, plan, participants};
            allocations_before_failure = -1;
            require(runner.find({1})->instrument().symbol == configs[1].symbol && runner.queue(0)->empty() &&
                runner.queue(1)->empty() && !runner.queue(2), "constructed partitions differ");
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            failed = true;
            ++failures;
        }
        require(registry.find({1}) == borrowed && borrowed->symbol == configs[1].symbol &&
            plan.assignments().data() == assignment, "failed constructor changed source configuration");
        PartitionedSimulation retry{registry, plan, participants};
        require(retry.find({1})->book().empty() && retry.find({2})->book().empty() &&
            retry.queue(0)->last_sequence().value == 0, "constructor retry retained state");
        if (!failed) break;
    }
    require(failures != 0, "partition constructor faults not exercised");
}

void test_schedule_failure() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 2, worker_bindings(registry, 2)};
    for (const bool populated : {false, true}) {
        PartitionedSimulation runner{registry, plan, participants};
        if (populated) require(runner.schedule(cancel(1, 7, 1, 1)).has_value(), "pending fixture failed");
        const auto* queue = runner.queue(0);
        const auto before_a = state(*runner.find({1})), before_b = state(*runner.find({2}));
        // Reach an actual growth allocation without assuming a vector growth factor.
        for (;;) {
            const auto* head = queue->next();
            const auto sequence = queue->last_sequence();
            const auto size = queue->size();
            allocations_before_failure = 0;
            bool failed = false;
            try {
                const auto scheduled = runner.schedule(order(1, 1, 1, Side::Buy, 100, 5, 0));
                allocations_before_failure = -1;
                require(scheduled && scheduled->value == sequence.value + 1, "non-growing schedule failed");
            } catch (const std::bad_alloc&) {
                allocations_before_failure = -1;
                failed = true;
            }
            if (!failed) continue;
            require(queue->next() == head && queue->size() == size && queue->last_sequence() == sequence &&
                state(*runner.find({1})) == before_a && state(*runner.find({2})) == before_b,
                "schedule failure changed queue, sequence or either book");
            const auto retry = runner.schedule(order(1, 1, 1, Side::Buy, 100, 5, 0));
            require(retry && retry->value == sequence.value + 1 && runner.step(0)->has_value(),
                "schedule retry lost an arrival or consumed sequence twice");
            break;
        }
    }
}

void test_step_failure_and_retry() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 3, worker_bindings(registry, 3)};
    for (int scenario = 0; scenario < 6; ++scenario) {
        PartitionedSimulation baseline{registry, plan, participants};
        prepare(baseline);
        require(baseline.schedule(pending_request(scenario)).has_value(), "baseline scheduling failed");
        const auto expected = baseline.step(0);
        require(expected && *expected, "baseline step failed");
        const auto expected_events = event_text((**expected).events);
        const auto expected_state = state(*baseline.find({1}));
        std::size_t failures = 0;
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            PartitionedSimulation runner{registry, plan, participants};
            prepare(runner);
            require(runner.schedule(pending_request(scenario)).has_value() &&
                runner.schedule(cancel(2, 1, 2, 8)).has_value(), "fault pending preparation failed");
            const auto* queue = runner.queue(0);
            const auto* head = queue->next();
            const auto before_a = state(*runner.find({1})), before_b = state(*runner.find({2}));
            const auto* foreign_head = runner.queue(1)->next();
            bool failed = false;
            allocations_before_failure = fault;
            try {
                const auto result = runner.step(0);
                allocations_before_failure = -1;
                require(result && *result && event_text((**result).events) == expected_events &&
                    (**result).arrival.sequence == (**expected).arrival.sequence,
                    "successful partitioned result changed");
            } catch (const std::bad_alloc&) {
                allocations_before_failure = -1;
                failed = true;
                ++failures;
            }
            if (failed) {
                require(queue->next() == head && queue->size() == 1 &&
                    state(*runner.find({1})) == before_a && state(*runner.find({2})) == before_b,
                    "step failure changed pending head or committed book/clock/counters");
                const auto retry = runner.step(0);
                require(retry && *retry && event_text((**retry).events) == expected_events &&
                    (**retry).arrival.sequence == (**expected).arrival.sequence, "identical step retry differs");
            }
            require(queue->empty() && state(*runner.find({1})) == expected_state &&
                state(*runner.find({2})) == before_b && runner.queue(1)->next() == foreign_head &&
                runner.queue(1)->size() == 1, "partition commit changed another worker or consumed arrival twice");
            if (!failed) break;
        }
        require(failures != 0, "partition step allocation failures not exercised");
    }
}

void test_nonallocating_paths() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs}, empty;
    WorkerAssignmentPlan plan{registry, 3, worker_bindings(registry, 3)}, empty_plan{empty, 3, {}};
    PartitionedSimulation runner{registry, plan, participants};
    prepare(runner);
    const auto* a = runner.find({1});
    const auto before = state(*a);
    allocations_before_failure = 0;
    PartitionedSimulation idle{empty, empty_plan, participants};
    const auto* found = runner.find({1});
    const auto* missing = runner.find({99});
    const auto* queue = runner.queue(0);
    const auto owner = runner.plan().worker_for({1});
    const auto absent = runner.schedule(cancel(99, 1, 1, 0));
    const auto past = runner.schedule(cancel(1, 1, 1, 0));
    const auto drained = runner.step(0), unused = runner.step(2), invalid = runner.step(3);
    allocations_before_failure = -1;
    require(found == a && !missing && queue->empty() && owner == 0 &&
        absent.error() == PartitionedScheduleError::UnknownInstrument &&
        past.error() == PartitionedScheduleError::InvalidTimestamp && drained && !*drained &&
        unused && !*unused && invalid.error() == PartitionedStepError::InvalidWorker && state(*a) == before &&
        !idle.queue(0), "empty/read/refusal path allocated or changed state");
}

int main() {
    try {
        test_constructor_failure();
        test_schedule_failure();
        test_step_failure_and_retry();
        test_nonallocating_paths();
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
