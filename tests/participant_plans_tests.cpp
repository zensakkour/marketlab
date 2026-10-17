#include "participant_test_support.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <vector>

using namespace participant_test;

void require_unchanged(const SimulationRandom& random, const SimulationRandom& before,
    const OrderIdSource& ids, std::optional<OrderId> next) {
    require(same_random(random, before) && ids.peek() == next, "failed plan consumed random state or IDs");
}

void test_noise_policy_and_replay() {
    auto config = noise();
    config.flow = {{1}, {2}, {10}, 3, 2, 2, 3, 3, 10000};
    config.maximum_distance_ticks = 0;
    config.cancel_basis_points = 10000;
    SimulationRandom random{42};
    const auto before = random;
    OrderIdSource ids;
    const auto plan = make_noise_script(config, random, ids);
    const ParticipantPlan expected{
        OrderRequest{{10}, {1}, {2}, {1}, Side::Buy, OrderType::Limit, 3, 100},
        CancelRequest{{12}, {1}, {2}, {1}},
        OrderRequest{{14}, {1}, {2}, {2}, Side::Buy, OrderType::Limit, 3, 100}};
    require(plan && same_plan(*plan, expected) && ids.peek() == OrderId{3} && same_random(random, before),
        "noise cancellation policy, endpoint probabilities or singleton consumption differs");

    config = noise();
    config.flow.action_count = 1000;
    SimulationRandom first{123};
    SimulationRandom second{123};
    OrderIdSource first_ids{{10}};
    OrderIdSource second_ids{{10}};
    const auto generated = make_noise_script(config, first, first_ids);
    const auto replay = make_noise_script(config, second, second_ids);
    require(generated && replay && same_plan(*generated, *replay) && same_random(first, second) &&
        first_ids.peek() == second_ids.peek(), "noise replay differs");
    std::set<OrderId> candidates;
    std::size_t cancellations = 0;
    std::size_t buys = 0;
    std::size_t sells = 0;
    auto previous = config.flow.first_time;
    for (std::size_t index = 0; index < generated->size(); ++index) {
        std::visit([&](const auto& request) {
            require(!validate_fields(request) && request.participant_id == ParticipantId{2} &&
                request.instrument_id == InstrumentId{1}, "generated noise fields invalid");
            if (index == 0) require(request.timestamp == previous, "first noise time differs");
            else require(request.timestamp.nanoseconds - previous.nanoseconds >= 1 &&
                request.timestamp.nanoseconds - previous.nanoseconds <= 5, "noise interval outside configured range");
            previous = request.timestamp;
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                require(candidates.insert(request.order_id).second && request.quantity_units >= 1 &&
                    request.quantity_units <= 4 && request.limit_price_ticks >= 96 && request.limit_price_ticks <= 104,
                    "noise order range or uniqueness differs");
                if (request.side == Side::Buy) {
                    ++buys;
                    require(*request.limit_price_ticks <= 100, "noise buy exceeds reference");
                } else {
                    ++sells;
                    require(*request.limit_price_ticks >= 100, "noise sell below reference");
                }
            } else {
                ++cancellations;
                require(candidates.erase(request.target_order_id) == 1, "noise cancelled a foreign or repeated target");
            }
        }, (*generated)[index]);
    }
    require(buys != 0 && sells != 0 && cancellations != 0, "noise workload missed a request kind");
}

void test_provider_and_taker() {
    OrderIdSource ids;
    auto config = provider();
    const auto plan = make_liquidity_script(config, ids);
    require(plan && plan->size() == 10 && ids.peek() == OrderId{7}, "provider plan size/IDs differ");
    const ParticipantPlan expected{
        OrderRequest{{0}, {1}, {1}, {1}, Side::Buy, OrderType::Limit, 5, 98},
        OrderRequest{{0}, {1}, {1}, {2}, Side::Sell, OrderType::Limit, 5, 102},
        CancelRequest{{10}, {1}, {1}, {1}}, CancelRequest{{10}, {1}, {1}, {2}},
        OrderRequest{{10}, {1}, {1}, {3}, Side::Buy, OrderType::Limit, 5, 98},
        OrderRequest{{10}, {1}, {1}, {4}, Side::Sell, OrderType::Limit, 5, 102},
        CancelRequest{{20}, {1}, {1}, {3}}, CancelRequest{{20}, {1}, {1}, {4}},
        OrderRequest{{20}, {1}, {1}, {5}, Side::Buy, OrderType::Limit, 5, 98},
        OrderRequest{{20}, {1}, {1}, {6}, Side::Sell, OrderType::Limit, 5, 102}};
    require(same_plan(*plan, expected), "provider refresh order or prices differ");

    auto taker = flow();
    taker.participant_id = {3};
    taker.action_count = 100;
    SimulationRandom random{42};
    SimulationRandom replay_random{42};
    auto replay_ids = ids;
    const auto market = make_taker_script(taker, random, ids);
    const auto replay = make_taker_script(taker, replay_random, replay_ids);
    require(market && replay && same_plan(*market, *replay) && ids.peek() == OrderId{107}, "taker replay or shared IDs differ");
    auto previous = taker.first_time;
    for (std::size_t index = 0; index < market->size(); ++index) {
        const auto& request = std::get<OrderRequest>((*market)[index]);
        require(!validate_fields(request) && request.type == OrderType::Market &&
            !request.limit_price_ticks && request.participant_id == ParticipantId{3} &&
            request.order_id.value == 7 + index && request.quantity_units >= 1 && request.quantity_units <= 4,
            "taker generated invalid market fields or ranges");
        if (index == 0) require(request.timestamp == previous, "first taker time differs");
        else require(request.timestamp.nanoseconds - previous.nanoseconds >= 1 &&
            request.timestamp.nanoseconds - previous.nanoseconds <= 5, "taker interval outside configured range");
        previous = request.timestamp;
    }
}

void test_invalid_and_failed_plans() {
    for (int test = 0; test < 10; ++test) {
        auto config = noise();
        if (test == 0) config.flow.instrument_id = {0};
        if (test == 1) config.flow.participant_id = {0};
        if (test == 2) config.flow.minimum_interval_ns = 0;
        if (test == 3) config.flow.maximum_interval_ns = 0;
        if (test == 4) config.flow.minimum_quantity = 0;
        if (test == 5) config.flow.maximum_quantity = 0;
        if (test == 6) config.flow.buy_basis_points = 10001;
        if (test == 7) config.maximum_distance_ticks = -1;
        if (test == 8) config.maximum_distance_ticks = 100;
        if (test == 9) config.cancel_basis_points = 10001;
        SimulationRandom random{42};
        const auto before = random;
        OrderIdSource ids;
        const auto result = make_noise_script(config, random, ids);
        require(!result && result.error() == ParticipantPlanError::InvalidConfiguration, "invalid noise configuration accepted");
        require_unchanged(random, before, ids, OrderId{1});
        if (test < 7) {
            const auto taker = make_taker_script(config.flow, random, ids);
            require(!taker && taker.error() == ParticipantPlanError::InvalidConfiguration,
                "invalid taker configuration accepted");
            require_unchanged(random, before, ids, OrderId{1});
        }
    }

    for (int test = 0; test < 7; ++test) {
        auto config = provider();
        if (test == 0) config.instrument_id = {0};
        if (test == 1) config.participant_id = {0};
        if (test == 2) config.quote_quantity = 0;
        if (test == 3) config.refresh_interval_ns = 0;
        if (test == 4) config.quote_distance_ticks = -1;
        if (test == 5) config.quote_distance_ticks = 100;
        if (test == 6) config.reference_price = Price::from_ticks(INT64_MAX).value();
        config.refresh_count = 0;
        OrderIdSource ids;
        const auto result = make_liquidity_script(config, ids);
        require(!result && result.error() == ParticipantPlanError::InvalidConfiguration &&
            ids.peek() == OrderId{1}, "invalid empty provider configuration accepted or consumed IDs");
    }

    for (int kind = 0; kind < 3; ++kind) {
        SimulationRandom random{42};
        const auto before = random;
        OrderIdSource ids;
        auto noise_config = noise();
        auto taker = flow();
        auto liquidity = provider();
        noise_config.flow.first_time = {UINT64_MAX};
        taker.first_time = {UINT64_MAX};
        liquidity.first_time = {UINT64_MAX};
        const auto result = kind == 0 ? make_noise_script(noise_config, random, ids) :
            kind == 1 ? make_taker_script(taker, random, ids) : make_liquidity_script(liquidity, ids);
        require(!result && result.error() == ParticipantPlanError::TimeOverflow, "plan time overflow wrapped");
        require_unchanged(random, before, ids, OrderId{1});

        noise_config = noise();
        noise_config.cancel_basis_points = 0;
        OrderIdSource exhausted{{UINT64_MAX}};
        const auto out_of_ids = kind == 0 ? make_noise_script(noise_config, random, exhausted) :
            kind == 1 ? make_taker_script(flow(), random, exhausted) : make_liquidity_script(provider(), exhausted);
        require(!out_of_ids && out_of_ids.error() == ParticipantPlanError::OrderIdExhausted, "plan ID exhaustion wrapped");
        require_unchanged(random, before, exhausted, OrderId{UINT64_MAX});

        noise_config.flow.action_count = std::numeric_limits<std::size_t>::max();
        taker = flow();
        taker.action_count = std::numeric_limits<std::size_t>::max();
        liquidity = provider();
        liquidity.refresh_count = std::numeric_limits<std::size_t>::max();
        const auto oversized = kind == 0 ? make_noise_script(noise_config, random, ids) :
            kind == 1 ? make_taker_script(taker, random, ids) : make_liquidity_script(liquidity, ids);
        require(!oversized && oversized.error() == ParticipantPlanError::CapacityExceeded, "oversized plan not diagnosed");
        require_unchanged(random, before, ids, OrderId{1});
    }
}

void test_empty_and_boundaries() {
    SimulationRandom random{42};
    const auto before = random;
    OrderIdSource ids;
    auto config = noise();
    config.flow.action_count = 0;
    auto liquidity = provider();
    liquidity.refresh_count = 0;
    require(make_noise_script(config, random, ids)->empty() && make_taker_script(config.flow, random, ids)->empty() &&
        make_liquidity_script(liquidity, ids)->empty(), "empty plan differs");
    require_unchanged(random, before, ids, OrderId{1});

    auto taker = flow();
    taker.action_count = 1;
    taker.first_time = {UINT64_MAX};
    taker.minimum_quantity = taker.maximum_quantity = std::numeric_limits<std::int64_t>::max();
    taker.buy_basis_points = 0;
    OrderIdSource final{{UINT64_MAX}};
    const auto last = make_taker_script(taker, random, final);
    require(last && !final.peek() && std::get<OrderRequest>(last->front()).order_id == OrderId{UINT64_MAX} &&
        std::get<OrderRequest>(last->front()).timestamp == LogicalTime{UINT64_MAX} &&
        std::get<OrderRequest>(last->front()).side == Side::Sell, "final ID/time or maximum quantity differs");

    config = noise();
    config.flow.action_count = 1;
    config.maximum_distance_ticks = 0;
    for (const auto ticks : {std::int64_t{1}, std::numeric_limits<std::int64_t>::max()}) {
        config.reference_price = Price::from_ticks(ticks).value();
        require(make_noise_script(config, random, ids).has_value(), "boundary reference with zero distance refused");
        config.maximum_distance_ticks = 1;
        const auto invalid = make_noise_script(config, random, ids);
        require(!invalid && invalid.error() == ParticipantPlanError::InvalidConfiguration, "out-of-range quote reference accepted");
        config.maximum_distance_ticks = 0;
    }
    liquidity = provider();
    liquidity.refresh_count = 1;
    OrderIdSource pair{{UINT64_MAX - 1}};
    require(make_liquidity_script(liquidity, pair).has_value() && !pair.peek(), "provider final ID pair differs");
    liquidity.quote_distance_ticks = 0;
    require(!make_liquidity_script(liquidity, ids), "zero-spread provider configuration accepted");
}

int main() {
    try {
        test_noise_policy_and_replay();
        test_provider_and_taker();
        test_invalid_and_failed_plans();
        test_empty_and_boundaries();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
