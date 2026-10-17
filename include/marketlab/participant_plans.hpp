#pragma once

#include <marketlab/order_id_source.hpp>
#include <marketlab/simulation_event.hpp>
#include <marketlab/simulation_random.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace marketlab {

using ParticipantPlan = std::vector<SimulationRequest>;

enum class ParticipantPlanError {
    InvalidConfiguration,
    CapacityExceeded,
    TimeOverflow,
    OrderIdExhausted,
};

struct OrderFlowConfig {
    InstrumentId instrument_id{};
    ParticipantId participant_id{};
    LogicalTime first_time{};
    std::size_t action_count{};
    std::uint64_t minimum_interval_ns{1};
    std::uint64_t maximum_interval_ns{1};
    std::int64_t minimum_quantity{1};
    std::int64_t maximum_quantity{1};
    std::uint32_t buy_basis_points{5000};
};

struct NoiseConfig {
    OrderFlowConfig flow;
    Price reference_price;
    std::int64_t maximum_distance_ticks{};
    std::uint32_t cancel_basis_points{};
};

struct LiquidityProviderConfig {
    InstrumentId instrument_id;
    ParticipantId participant_id;
    Price reference_price;
    std::int64_t quote_distance_ticks;
    std::int64_t quote_quantity;
    LogicalTime first_time;
    std::uint64_t refresh_interval_ns;
    std::size_t refresh_count;
};

namespace detail {

[[nodiscard]] inline bool valid_flow(const OrderFlowConfig& config) noexcept {
    return is_valid(config.instrument_id) && is_valid(config.participant_id) &&
        config.minimum_interval_ns != 0 && config.minimum_interval_ns <= config.maximum_interval_ns &&
        config.minimum_quantity > 0 && config.minimum_quantity <= config.maximum_quantity &&
        config.buy_basis_points <= 10000;
}

[[nodiscard]] inline bool valid_reference(Price reference, std::int64_t distance) noexcept {
    return distance >= 0 && distance < reference.ticks() &&
        distance <= std::numeric_limits<std::int64_t>::max() - reference.ticks();
}

[[nodiscard]] inline bool draw_probability(SimulationRandom& random, std::uint32_t basis_points) noexcept {
    if (basis_points == 0) return false;
    if (basis_points == 10000) return true;
    return *random.uniform(1, 10000) <= basis_points;
}

[[nodiscard]] inline std::optional<LogicalTime> advance_time(LogicalTime time, std::uint64_t delay) noexcept {
    if (delay > std::numeric_limits<std::uint64_t>::max() - time.nanoseconds) {
        return std::nullopt;
    }
    return LogicalTime{time.nanoseconds + delay};
}

} // namespace detail

// Plans commit the experiment-owned sources only after complete successful construction.
[[nodiscard]] inline std::expected<ParticipantPlan, ParticipantPlanError> make_noise_script(
    const NoiseConfig& config, SimulationRandom& random, OrderIdSource& ids) {
    const auto& flow = config.flow;
    if (!detail::valid_flow(flow) || !detail::valid_reference(config.reference_price, config.maximum_distance_ticks) ||
        config.cancel_basis_points > 10000) {
        return std::unexpected{ParticipantPlanError::InvalidConfiguration};
    }
    ParticipantPlan plan;
    std::vector<OrderId> candidates;
    if (flow.action_count > plan.max_size() || flow.action_count > candidates.max_size()) {
        return std::unexpected{ParticipantPlanError::CapacityExceeded};
    }
    plan.reserve(flow.action_count);
    candidates.reserve(flow.action_count);
    auto prepared_random = random;
    auto prepared_ids = ids;
    auto time = flow.first_time;
    for (std::size_t index = 0; index < flow.action_count; ++index) {
        if (index != 0) {
            const auto next = detail::advance_time(time,
                *prepared_random.uniform(flow.minimum_interval_ns, flow.maximum_interval_ns));
            if (!next) return std::unexpected{ParticipantPlanError::TimeOverflow};
            time = *next;
        }
        if (!candidates.empty() && detail::draw_probability(prepared_random, config.cancel_basis_points)) {
            const auto selected = static_cast<std::size_t>(*prepared_random.uniform(0, candidates.size() - 1));
            plan.emplace_back(CancelRequest{time, flow.instrument_id, flow.participant_id, candidates[selected]});
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(selected));
            continue;
        }
        const auto id = prepared_ids.next();
        if (!id) return std::unexpected{ParticipantPlanError::OrderIdExhausted};
        const auto side = detail::draw_probability(prepared_random, flow.buy_basis_points) ? Side::Buy : Side::Sell;
        const auto quantity = static_cast<std::int64_t>(*prepared_random.uniform(
            static_cast<std::uint64_t>(flow.minimum_quantity), static_cast<std::uint64_t>(flow.maximum_quantity)));
        const auto distance = static_cast<std::int64_t>(*prepared_random.uniform(0,
            static_cast<std::uint64_t>(config.maximum_distance_ticks)));
        const auto ticks = side == Side::Buy ? config.reference_price.ticks() - distance :
            config.reference_price.ticks() + distance;
        plan.emplace_back(OrderRequest{time, flow.instrument_id, flow.participant_id, *id, side,
            OrderType::Limit, quantity, ticks});
        candidates.push_back(*id);
    }
    static_assert(std::is_nothrow_copy_assignable_v<SimulationRandom>);
    static_assert(std::is_nothrow_copy_assignable_v<OrderIdSource>);
    static_assert(std::is_nothrow_constructible_v<
        std::expected<ParticipantPlan, ParticipantPlanError>, ParticipantPlan&&>);
    random = prepared_random;
    ids = prepared_ids;
    return plan;
}

[[nodiscard]] inline std::expected<ParticipantPlan, ParticipantPlanError> make_taker_script(
    const OrderFlowConfig& config, SimulationRandom& random, OrderIdSource& ids) {
    if (!detail::valid_flow(config)) {
        return std::unexpected{ParticipantPlanError::InvalidConfiguration};
    }
    ParticipantPlan plan;
    if (config.action_count > plan.max_size()) {
        return std::unexpected{ParticipantPlanError::CapacityExceeded};
    }
    plan.reserve(config.action_count);
    auto prepared_random = random;
    auto prepared_ids = ids;
    auto time = config.first_time;
    for (std::size_t index = 0; index < config.action_count; ++index) {
        if (index != 0) {
            const auto next = detail::advance_time(time,
                *prepared_random.uniform(config.minimum_interval_ns, config.maximum_interval_ns));
            if (!next) return std::unexpected{ParticipantPlanError::TimeOverflow};
            time = *next;
        }
        const auto id = prepared_ids.next();
        if (!id) return std::unexpected{ParticipantPlanError::OrderIdExhausted};
        const auto side = detail::draw_probability(prepared_random, config.buy_basis_points) ? Side::Buy : Side::Sell;
        const auto quantity = static_cast<std::int64_t>(*prepared_random.uniform(
            static_cast<std::uint64_t>(config.minimum_quantity), static_cast<std::uint64_t>(config.maximum_quantity)));
        plan.emplace_back(OrderRequest{time, config.instrument_id, config.participant_id, *id, side,
            OrderType::Market, quantity, std::nullopt});
    }
    random = prepared_random;
    ids = prepared_ids;
    return plan;
}

[[nodiscard]] inline std::expected<ParticipantPlan, ParticipantPlanError> make_liquidity_script(
    const LiquidityProviderConfig& config, OrderIdSource& ids) {
    if (!is_valid(config.instrument_id) || !is_valid(config.participant_id) ||
        config.quote_distance_ticks <= 0 || !detail::valid_reference(config.reference_price, config.quote_distance_ticks) ||
        config.quote_quantity <= 0 || config.refresh_interval_ns == 0) {
        return std::unexpected{ParticipantPlanError::InvalidConfiguration};
    }
    ParticipantPlan plan;
    if (config.refresh_count == 0) return plan;
    if (plan.max_size() < 2 || config.refresh_count - 1 > (plan.max_size() - 2) / 4) {
        return std::unexpected{ParticipantPlanError::CapacityExceeded};
    }
    plan.reserve(2 + 4 * (config.refresh_count - 1));
    auto prepared_ids = ids;
    auto time = config.first_time;
    OrderId previous_bid{};
    OrderId previous_ask{};
    for (std::size_t index = 0; index < config.refresh_count; ++index) {
        if (index != 0) {
            const auto next = detail::advance_time(time, config.refresh_interval_ns);
            if (!next) return std::unexpected{ParticipantPlanError::TimeOverflow};
            time = *next;
            plan.emplace_back(CancelRequest{time, config.instrument_id, config.participant_id, previous_bid});
            plan.emplace_back(CancelRequest{time, config.instrument_id, config.participant_id, previous_ask});
        }
        const auto bid = prepared_ids.next();
        const auto ask = prepared_ids.next();
        if (!bid || !ask) return std::unexpected{ParticipantPlanError::OrderIdExhausted};
        plan.emplace_back(OrderRequest{time, config.instrument_id, config.participant_id, *bid, Side::Buy,
            OrderType::Limit, config.quote_quantity, config.reference_price.ticks() - config.quote_distance_ticks});
        plan.emplace_back(OrderRequest{time, config.instrument_id, config.participant_id, *ask, Side::Sell,
            OrderType::Limit, config.quote_quantity, config.reference_price.ticks() + config.quote_distance_ticks});
        previous_bid = *bid;
        previous_ask = *ask;
    }
    ids = prepared_ids;
    return plan;
}

} // namespace marketlab
