#include "participant_test_support.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <type_traits>
#include <vector>

using namespace participant_test;

static_assert(!std::is_copy_constructible_v<ScriptedParticipant>);
static_assert(!std::is_move_constructible_v<ScriptedParticipant>);

void test_configuration_and_ownership() {
    ScriptedParticipant empty{{1}, {}};
    require(empty.empty() && !empty.next() && empty.remaining() == 0 && empty.submitted_count() == 0 &&
        empty.participant_id() == ParticipantId{1}, "empty script differs");
    auto simulation = make_simulation();
    const auto exhausted = empty.schedule_next(simulation);
    require(exhausted && !*exhausted && simulation.queue().empty(), "empty script scheduled a request");
    for (int test = 0; test < 3; ++test) {
        ParticipantPlan script{
            OrderRequest{{1}, {1}, {1}, {9}, Side::Buy, OrderType::Limit, 2, 100},
            CancelRequest{{2}, {1}, {1}, {9}}};
        auto owner = ParticipantId{1};
        if (test == 0) owner = {0};
        if (test == 1) std::get<CancelRequest>(script[1]).participant_id = {2};
        if (test == 2) std::get<CancelRequest>(script[1]).timestamp = {0};
        bool refused = false;
        try { ScriptedParticipant invalid{owner, script}; } catch (const std::invalid_argument&) { refused = true; }
        require(refused, "invalid script configuration accepted");
    }

    ParticipantPlan source{OrderRequest{{0}, {0}, {1}, {0}, Side{}, OrderType{}, -7, 0}};
    ScriptedParticipant raw{{1}, source};
    source.clear();
    const auto* borrowed = raw.next();
    const auto scheduled = raw.schedule_next(simulation);
    require(scheduled && *scheduled && raw.empty() && raw.submitted_count() == 1 &&
        std::get<OrderRequest>(*borrowed).quantity_units == -7 && simulation.exchange().last_event_sequence().value == 0,
        "script borrowed caller data, changed raw fields or processed exchange state");
    const auto step = simulation.step();
    require(step && *step && std::get<OrderRejected>(step->value().events.front()).reason ==
        RejectionReason::InvalidInstrument, "raw script bypassed exchange validation");
}

void test_fifo_and_retry() {
    auto simulation = make_simulation();
    const ParticipantPlan actions{
        OrderRequest{{1}, {1}, {1}, {900}, Side::Buy, OrderType::Limit, 2, 100},
        OrderRequest{{1}, {1}, {1}, {1}, Side::Buy, OrderType::Limit, 3, 100},
        CancelRequest{{1}, {1}, {1}, {900}}};
    ScriptedParticipant actor{{1}, actions};
    for (std::uint64_t sequence = 1; sequence <= actions.size(); ++sequence) {
        const auto result = actor.schedule_next(simulation);
        require(result && *result && result->value().value == sequence, "script insertion sequence differs");
    }
    require(simulation.exchange().book().order_count() == 0 && actor.empty(), "script dispatched directly into book");
    for (const auto& expected : actions) {
        const auto delivered = simulation.step();
        require(delivered && *delivered && same_request(delivered->value().arrival.request, expected),
            "equal-time script order changed");
    }
    require(simulation.exchange().book().best_bid()->order.order_id == OrderId{1}, "script cancellation/FIFO differs");

    const ParticipantPlan past{OrderRequest{{0}, {1}, {1}, {10}, Side::Buy, OrderType::Limit, 1, 100}};
    ScriptedParticipant retry{{1}, past};
    const auto* next = retry.next();
    const auto count = simulation.queue().last_sequence();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto refused = retry.schedule_next(simulation);
        require(!refused && refused.error() == SimulationScheduleError::InvalidTimestamp &&
            retry.next() == next && retry.submitted_count() == 0 && simulation.queue().last_sequence() == count,
            "failed scheduling consumed a script action or sequence");
    }
    auto fresh = make_simulation();
    require(retry.schedule_next(fresh).has_value() && retry.empty(), "retained script action could not be retried");

    const ParticipantPlan unregistered{CancelRequest{{2}, {1}, {99}, {900}}};
    ScriptedParticipant unknown{{99}, unregistered};
    require(unknown.schedule_next(simulation).has_value(), "script applied exchange registration rules");
    const auto rejected = simulation.step();
    require(rejected && *rejected && std::get<CancelRejected>(rejected->value().events.front()).reason ==
        RejectionReason::InvalidParticipant, "exchange did not reject unregistered script owner");
}

int main() {
    try {
        test_configuration_and_ownership();
        test_fifo_and_retry();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
