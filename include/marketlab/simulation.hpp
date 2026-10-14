#pragma once

#include <marketlab/exchange.hpp>
#include <marketlab/simulation_queue.hpp>

#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace marketlab {

enum class SimulationScheduleError {
    InvalidTimestamp,
    SequenceExhausted,
    CapacityExceeded,
};

struct SimulationStep {
    SimulationEvent arrival;
    std::vector<ExchangeEvent> events;
};

class Simulation {
public:
    using StepResult = std::expected<std::optional<SimulationStep>, ExchangeError>;

    Simulation(Instrument instrument, std::span<const ParticipantId> participants)
        : exchange_(std::move(instrument), participants) {}

    [[nodiscard]] const Exchange& exchange() const noexcept {
        return exchange_;
    }

    [[nodiscard]] const SimulationQueue& queue() const noexcept {
        return queue_;
    }

    [[nodiscard]] LogicalTime current_time() const noexcept {
        return exchange_.current_time();
    }

    [[nodiscard]] std::expected<SimulationSequence, SimulationScheduleError> schedule(SimulationRequest request) {
        const auto time = std::visit([](const auto& record) { return record.timestamp; }, request);
        if (time < current_time()) {
            return std::unexpected{SimulationScheduleError::InvalidTimestamp};
        }
        const auto result = queue_.schedule(std::move(request));
        if (result) {
            return *result;
        }
        switch (result.error()) {
        case SimulationQueueError::SequenceExhausted:
            return std::unexpected{SimulationScheduleError::SequenceExhausted};
        case SimulationQueueError::CapacityExceeded:
            return std::unexpected{SimulationScheduleError::CapacityExceeded};
        }
        throw std::logic_error{"queue returned an unknown scheduling error"};
    }

    // Exchange failures preserve the pending head; only committed processing removes it.
    [[nodiscard]] StepResult step() {
        const auto* arrival = queue_.next();
        if (!arrival) {
            return std::nullopt;
        }
        auto events = std::visit([&](const auto& request) {
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                return exchange_.process_order(request);
            } else {
                return exchange_.process_cancel(request);
            }
        }, arrival->request);
        if (!events) {
            return std::unexpected{events.error()};
        }
        // After exchange commit, removal and result transfer must not fail.
        static_assert(std::is_nothrow_constructible_v<StepResult, SimulationStep&&>);
        auto removed = queue_.pop_next();
        return SimulationStep{std::move(*removed), std::move(*events)};
    }

private:
    Exchange exchange_;
    SimulationQueue queue_;
};

} // namespace marketlab
