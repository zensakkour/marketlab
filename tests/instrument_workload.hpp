#pragma once

#include "multi_instrument_test_support.hpp"

#include <marketlab/simulation_random.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace multi_test {

inline std::vector<Instrument> workload_instruments(std::size_t count) {
    std::vector<Instrument> result;
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(Instrument{{index + 1}, "SYMBOL" + std::to_string(index + 1),
            TickSize::from_decimal(static_cast<std::int64_t>(index + 1), 2).value()});
    }
    return result;
}

// Each instrument's input times are nondecreasing, but independent clocks differ.
// Reused IDs, self-matches, malformed fields and uneven activity exercise isolation.
inline std::vector<SimulationRequest> instrument_workload(
    std::uint64_t seed, std::size_t instrument_count, std::size_t request_count) {
    SimulationRandom random{seed};
    std::vector<std::uint64_t> times(instrument_count);
    std::vector<SimulationRequest> result;
    for (std::size_t index = 0; index < request_count; ++index) {
        const auto symbol = *random.uniform(0, 9) < 8 ? 1 : *random.uniform(1, instrument_count);
        auto& time = times[symbol - 1];
        time += *random.uniform(0, 1);
        const auto id = *random.uniform(1, 64);
        const auto owner = *random.uniform(1, 2);
        const auto category = *random.uniform(0, 9);
        if (category < 3) {
            result.emplace_back(cancel(symbol, id, owner, time));
        } else {
            // Sequence random draws explicitly; function argument order is unspecified.
            const auto units = static_cast<std::int64_t>(*random.uniform(1, 20));
            const auto price = static_cast<std::int64_t>(*random.uniform(90, 110));
            const auto side = *random.uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
            auto request = order(symbol, id, owner, side, price, units, time);
            if (category == 3 || category == 4) {
                request.type = OrderType::Market;
                request.limit_price_ticks.reset();
            }
            if (category == 8) request.quantity_units = 0;
            if (category == 9) request.participant_id = {99};
            result.emplace_back(request);
        }
    }
    return result;
}

inline std::vector<Execution> executions_in(std::span<const ExchangeEvent> events) {
    std::vector<Execution> result;
    for (const auto& event : events) {
        if (const auto* execution = std::get_if<Execution>(&event)) result.push_back(*execution);
    }
    return result;
}

} // namespace multi_test
