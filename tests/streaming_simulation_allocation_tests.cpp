#include "partitioned_test_support.hpp"

#include <marketlab/streaming_simulation.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
std::ptrdiff_t budget = -1;
}

#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void* operator new(std::size_t bytes) {
    if (budget >= 0 && budget-- == 0) throw std::bad_alloc{};
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

void test_constructor_failure() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 3, worker_bindings(registry, 3)};
    const auto* record = registry.find({1});
    const auto* bindings = plan.assignments().data();
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        bool failed = false;
        budget = fault;
        try { StreamingSimulation owner{registry, plan, participants, 3}; }
        catch (const std::bad_alloc&) { failed = true; ++failures; }
        budget = -1;
        StreamingSimulation retry{registry, plan, participants, 3};
        require(registry.find({1}) == record && plan.assignments().data() == bindings &&
            retry.queue(0)->size() == 0 && retry.queue(1)->capacity() == 3 && !retry.is_closed() &&
            retry.try_schedule(cancel(1, 1, 1, 0))->value == 1, "constructor retry retained state or changed source views");
        if (!failed) break;
    }
    require(failures != 0, "streaming construction allocation failures were not exercised");
}

void prepare(Exchange& exchange) {
    require(exchange.process_order(order(1, 1, 1, Side::Sell, 100, 5, 0)).has_value() &&
        exchange.process_order(order(1, 2, 1, Side::Sell, 101, 7, 1)).has_value(), "reference setup failed");
}

void prepare(StreamingSimulation& owner) {
    for (const auto& request : {order(1, 1, 1, Side::Sell, 100, 5, 0), order(1, 2, 1, Side::Sell, 101, 7, 1)}) {
        require(owner.try_schedule(request).has_value() && owner.step(0).has_value(), "streaming setup failed");
    }
}

void test_pending_retry_and_independent_worker() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 2, worker_bindings(registry, 2)};
    auto market = order(1, 3, 2, Side::Buy, 0, 10, 2);
    market.type = OrderType::Market;
    market.limit_price_ticks.reset();
    const std::array<SimulationRequest, 6> cases{
        order(1, 3, 2, Side::Buy, 99, 4, 2), order(1, 3, 2, Side::Buy, 101, 10, 2), market,
        cancel(1, 1, 1, 2), order(1, 3, 2, Side::Buy, 99, 0, 2), cancel(1, 1, 2, 2)};
    const SimulationRequest successor = cancel(1, 2, 1, 3);
    for (const auto& request : cases) {
        Exchange reference{*registry.find({1}), participants};
        prepare(reference);
        const auto expected = isolated_process(reference, request);
        const auto expected_next = isolated_process(reference, successor);
        require(expected && expected_next, "failure reference processing failed");
        std::size_t failures = 0;
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            StreamingSimulation owner{registry, plan, participants, 1};
            prepare(owner);
            require(owner.try_schedule(request)->value == 3 &&
                owner.try_schedule(order(2, 1, 2, Side::Buy, 200, 4, 0))->value == 1, "failure fixture scheduling failed");
            const auto before = state(*owner.find({1}));
            const auto foreign_before = state(*owner.find({2}));
            std::optional<WorkerStep> committed;
            bool failed = false;
            budget = fault;
            try {
                auto step = owner.step(0);
                budget = -1;
                require(step.has_value(), "allocation became a streaming diagnostic");
                committed.emplace(std::move(*step));
            } catch (const std::bad_alloc&) {
                budget = -1;
                failed = true;
                ++failures;
            }
            require(owner.queue(0)->size() == 0 && state(*owner.find({2})) == foreign_before && owner.queue(1)->size() == 1,
                "step did not retain ownership outside ingress or changed a foreign partition");
            if (failed) require(!committed && state(*owner.find({1})) == before, "failed step changed exchange state");
            require(owner.try_schedule(successor)->value == 4 &&
                owner.try_schedule(successor).error() == StreamingScheduleError::Full,
                "in-flight storage did not preserve capacity+1 bound or refused push consumed a sequence");
            const auto healthy = owner.step(1);
            require(healthy && healthy->arrival.sequence.value == 1 && owner.find({2})->book().find({1}),
                "failed partition prevented independent worker completion");
            owner.close();
            if (failed) {
                auto retried = owner.step(0);
                require(retried.has_value(), "closed ingress prevented retry of an in-flight request");
                committed.emplace(std::move(*retried));
            }
            require(committed && committed->arrival.sequence.value == 3 && scheduled_time(committed->arrival) == LogicalTime{2} &&
                event_text(committed->events) == event_text(*expected) && owner.queue(0)->size() == 1,
                "pending retry lost its original sequence/events or let its successor overtake it");
            const auto next = owner.step(0);
            require(next && next->arrival.sequence.value == 4 && event_text(next->events) == event_text(*expected_next) &&
                state(*owner.find({1})) == state(reference) && owner.find({1})->book().check_invariants() &&
                owner.step(0).error() == StreamingStepError::Closed && owner.step(1).error() == StreamingStepError::Closed,
                "retry plus closed drain lost/duplicated arrivals or changed final state");
            if (!failed) break;
        }
        require(failures != 0, "pending-arrival allocation failures were not exercised");
    }
}

void test_nonallocating_ingress_and_empty_paths() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, 3, worker_bindings(registry, 3)};
    StreamingSimulation owner{registry, plan, participants, 1};
    budget = 0;
    const auto accepted = owner.try_schedule(cancel(1, 1, 1, 10));
    const auto full = owner.try_schedule(cancel(1, 1, 1, 11));
    const auto past = owner.try_schedule(cancel(1, 1, 1, 9));
    const auto unknown = owner.try_schedule(cancel(99, 1, 1, 0));
    const auto empty = owner.step(1), idle = owner.step(2), invalid = owner.step(3);
    const auto* exchange = owner.find({1});
    const auto* queue = owner.queue(0);
    const auto queued = queue->size(), capacity = queue->capacity();
    const auto binding = owner.plan().worker_for({1});
    owner.close();
    owner.close();
    const auto closed = owner.try_schedule(cancel(1, 1, 1, 10));
    const auto drained = owner.step(1), idle_closed = owner.step(2);
    const bool is_closed = owner.is_closed() && queue->is_closed();
    budget = -1;
    require(accepted && accepted->value == 1 && full.error() == StreamingScheduleError::Full &&
        past.error() == StreamingScheduleError::InvalidTimestamp && unknown.error() == StreamingScheduleError::UnknownInstrument &&
        empty.error() == StreamingStepError::Empty && idle.error() == StreamingStepError::Empty &&
        invalid.error() == StreamingStepError::InvalidWorker && exchange && queued == 1 && capacity == 1 && binding == 0 &&
        is_closed && closed.error() == StreamingScheduleError::Closed && drained.error() == StreamingStepError::Closed &&
        idle_closed.error() == StreamingStepError::Closed, "nonallocating streaming paths changed semantics or allocated");
    require(owner.step(0).has_value() && owner.step(0).error() == StreamingStepError::Closed, "read/refusal/close changed pending work");
}

int main() {
    try {
        test_constructor_failure();
        test_pending_retry_and_independent_worker();
        test_nonallocating_ingress_and_empty_paths();
    } catch (const std::exception& error) {
        budget = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
