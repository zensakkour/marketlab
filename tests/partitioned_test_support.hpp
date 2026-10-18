#pragma once

#include "instrument_workload.hpp"

#include <marketlab/partitioned_simulation.hpp>

namespace multi_test {

inline std::vector<InstrumentWorkerAssignment> worker_bindings(const InstrumentRegistry& registry, std::size_t workers) {
    std::vector<InstrumentWorkerAssignment> result;
    for (const auto& config : registry.instruments()) result.push_back({config.id, (config.id.value - 1) % workers});
    return result;
}

inline std::expected<std::vector<ExchangeEvent>, ExchangeError> isolated_process(Exchange& exchange, const SimulationRequest& request) {
    return std::visit([&](const auto& value) {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, OrderRequest>) return exchange.process_order(value);
        else return exchange.process_cancel(value);
    }, request);
}

} // namespace multi_test
