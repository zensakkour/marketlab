#include "participant_test_support.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>

namespace {
std::ptrdiff_t allocations_before_failure = -1;
}

void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) throw std::bad_alloc{};
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

using namespace participant_test;

std::expected<ParticipantPlan, ParticipantPlanError> build(int kind, SimulationRandom& random, OrderIdSource& ids) {
    if (kind == 0) return make_noise_script(noise(), random, ids);
    if (kind == 1) return make_taker_script(flow(), random, ids);
    return make_liquidity_script(provider(), ids);
}

void test_plan_failure_and_retry() {
    for (int kind = 0; kind < 3; ++kind) {
        SimulationRandom expected_random{42};
        OrderIdSource expected_ids{{41}};
        const auto expected = build(kind, expected_random, expected_ids);
        require(expected.has_value(), "plan allocation fixture failed");
        std::size_t failures = 0;
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            SimulationRandom random{42};
            const auto before = random;
            OrderIdSource ids{{41}};
            bool failed = false;
            allocations_before_failure = fault;
            try {
                const auto result = build(kind, random, ids);
                allocations_before_failure = -1;
                require(result && same_plan(*result, *expected), "successful plan differs");
            } catch (const std::bad_alloc&) {
                allocations_before_failure = -1;
                failed = true;
                ++failures;
            }
            if (failed) {
                require(same_random(random, before) && ids.peek() == OrderId{41},
                    "plan allocation failure consumed randomness or IDs");
                const auto retry = build(kind, random, ids);
                require(retry && same_plan(*retry, *expected), "plan allocation retry differs");
            }
            require(same_random(random, expected_random) && ids.peek() == expected_ids.peek(),
                "successful plan or retry committed the wrong source states");
            if (!failed) break;
        }
        require(failures != 0, "plan allocations were not exercised");
    }
}

void test_script_failure_and_retry() {
    const ParticipantPlan source{
        OrderRequest{{0}, {1}, {2}, {41}, Side::Buy, OrderType::Limit, 3, 100},
        CancelRequest{{1}, {1}, {2}, {41}}};
    bool failed = false;
    allocations_before_failure = 0;
    try { ScriptedParticipant actor{{2}, source}; } catch (const std::bad_alloc&) { failed = true; }
    allocations_before_failure = -1;
    require(failed && source.size() == 2, "script ownership allocation failure not exercised");

    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        auto simulation = make_simulation();
        ScriptedParticipant actor{{2}, source};
        const auto* borrowed = actor.next();
        bool scheduling_failed = false;
        allocations_before_failure = fault;
        try {
            const auto result = actor.schedule_next(simulation);
            allocations_before_failure = -1;
            require(result && *result, "valid script scheduling failed");
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            scheduling_failed = true;
            ++failures;
        }
        if (scheduling_failed) {
            require(actor.next() == borrowed && actor.submitted_count() == 0 && actor.remaining() == 2 &&
                simulation.queue().empty() && simulation.queue().last_sequence().value == 0 &&
                simulation.exchange().last_event_sequence().value == 0,
                "scheduling failure changed cursor, queue or exchange");
            require(actor.schedule_next(simulation).has_value(), "script scheduling retry failed");
        }
        require(actor.submitted_count() == 1 && actor.remaining() == 1 &&
            simulation.queue().size() == 1 && simulation.queue().last_sequence().value == 1 &&
            same_request(simulation.queue().next()->request, source.front()),
            "script scheduling retry lost or duplicated action");
        if (!scheduling_failed) break;
    }
    require(failures != 0, "script scheduling allocations were not exercised");
}

void test_no_allocation_paths() {
    SimulationRandom random{42};
    const auto before = random;
    OrderIdSource ids;
    auto config = noise();
    config.flow.action_count = 0;
    auto provider_config = provider();
    provider_config.refresh_count = 0;
    auto simulation = make_simulation();
    ScriptedParticipant empty{{2}, {}};
    allocations_before_failure = 0;
    const auto noise_plan = make_noise_script(config, random, ids);
    const auto taker_plan = make_taker_script(config.flow, random, ids);
    const auto provider_plan = make_liquidity_script(provider_config, ids);
    config.flow.minimum_quantity = 0;
    const auto invalid = make_noise_script(config, random, ids);
    const auto exhausted = empty.schedule_next(simulation);
    const auto next_id = ids.next();
    const auto sample = random.uniform(5, 5);
    allocations_before_failure = -1;
    require(noise_plan && noise_plan->empty() && taker_plan && taker_plan->empty() &&
        provider_plan && provider_plan->empty() && !invalid &&
        invalid.error() == ParticipantPlanError::InvalidConfiguration && exhausted && !*exhausted &&
        next_id == OrderId{1} && sample == 5 && same_random(random, before),
        "empty/invalid/source operation allocated or changed state");
}

int main() {
    try {
        test_plan_failure_and_retry();
        test_script_failure_and_retry();
        test_no_allocation_paths();
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
