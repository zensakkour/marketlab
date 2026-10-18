#pragma once

#include <marketlab/multi_instrument_exchange.hpp>
#include <marketlab/simulation_queue.hpp>
#include <marketlab/worker_assignment_plan.hpp>

#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace marketlab {

enum class PartitionedScheduleError {
    UnknownInstrument,
    InvalidTimestamp,
    SequenceExhausted,
    CapacityExceeded,
};

enum class PartitionedStepError {
    InvalidWorker,
    RequestSequenceExhausted,
    EventSequenceExhausted,
    OutputCapacityExceeded,
};

struct WorkerStep {
    std::size_t worker_index;
    SimulationEvent arrival;
    std::vector<ExchangeEvent> events;
};

namespace detail {
[[nodiscard]] constexpr PartitionedStepError partitioned_step_error(MultiInstrumentError error) {
    switch (error) {
    case MultiInstrumentError::RequestSequenceExhausted: return PartitionedStepError::RequestSequenceExhausted;
    case MultiInstrumentError::EventSequenceExhausted: return PartitionedStepError::EventSequenceExhausted;
    case MultiInstrumentError::OutputCapacityExceeded: return PartitionedStepError::OutputCapacityExceeded;
    case MultiInstrumentError::UnknownInstrument: break;
    }
    throw std::logic_error{"worker queue contains an unowned instrument"};
}

// Shared processing for independent owners; a worker's mutable state never escapes
// either runner. Scheduling and stepping must not overlap for the same worker.
struct SimulationWorker {
    using StepResult = std::expected<std::optional<WorkerStep>, PartitionedStepError>;

    SimulationWorker(std::span<const Instrument> instruments, std::span<const ParticipantId> participants)
        : exchange{InstrumentRegistry{instruments}, participants} {}

    [[nodiscard]] std::expected<SimulationSequence, PartitionedScheduleError> schedule(SimulationRequest request) {
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const auto* destination = exchange.find(id);
        if (!destination) return std::unexpected{PartitionedScheduleError::UnknownInstrument};
        const auto time = std::visit([](const auto& value) { return value.timestamp; }, request);
        if (time < destination->current_time()) return std::unexpected{PartitionedScheduleError::InvalidTimestamp};
        const auto result = queue.schedule(std::move(request));
        if (result) return *result;
        switch (result.error()) {
        case SimulationQueueError::SequenceExhausted: return std::unexpected{PartitionedScheduleError::SequenceExhausted};
        case SimulationQueueError::CapacityExceeded: return std::unexpected{PartitionedScheduleError::CapacityExceeded};
        }
        throw std::logic_error{"queue returned an unknown scheduling error"};
    }

    [[nodiscard]] StepResult step(std::size_t worker_index) {
        const auto* arrival = queue.next();
        if (!arrival) return std::nullopt;
        auto events = std::visit([&](const auto& request) {
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                return exchange.process_order(request);
            } else return exchange.process_cancel(request);
        }, arrival->request);
        if (!events) return std::unexpected{partitioned_step_error(events.error())};
        // After exchange commit, removal and owned result transfer cannot fail.
        static_assert(std::is_nothrow_constructible_v<StepResult, WorkerStep&&>);
        auto removed = queue.pop_next();
        return WorkerStep{worker_index, std::move(*removed), std::move(*events)};
    }

    MultiInstrumentExchange exchange;
    SimulationQueue queue;
};
} // namespace detail

// Caller-driven partitions. All calls, including reads, belong to one logical
// writer; independent step calls do not authorize concurrent access to this owner.
class PartitionedSimulation {
public:
    using StepResult = std::expected<std::optional<WorkerStep>, PartitionedStepError>;

    PartitionedSimulation(const InstrumentRegistry& registry, const WorkerAssignmentPlan& plan,
                          std::span<const ParticipantId> participants)
        : plan_{registry, plan.worker_count(), plan.assignments()} {
        for (const auto participant : participants) {
            if (!is_valid(participant)) throw std::invalid_argument{"simulation requires nonzero participant IDs"};
        }
        std::map<std::size_t, std::vector<Instrument>> grouped;
        for (const auto& instrument : registry.instruments()) {
            grouped[*plan_.worker_for(instrument.id)].push_back(instrument);
        }
        for (const auto& [index, instruments] : grouped) workers_.try_emplace(index, instruments, participants);
    }

    PartitionedSimulation(const PartitionedSimulation&) = delete;
    PartitionedSimulation& operator=(const PartitionedSimulation&) = delete;

    [[nodiscard]] const WorkerAssignmentPlan& plan() const noexcept { return plan_; }

    // Stable views until destruction. Idle/invalid worker indexes have no stored queue.
    [[nodiscard]] const SimulationQueue* queue(std::size_t worker_index) const noexcept {
        const auto worker = workers_.find(worker_index);
        return worker == workers_.end() ? nullptr : &worker->second.queue;
    }

    [[nodiscard]] const Exchange* find(InstrumentId id) const noexcept {
        const auto index = plan_.worker_for(id);
        if (!index) return nullptr;
        return workers_.find(*index)->second.exchange.find(id);
    }

    [[nodiscard]] std::expected<SimulationSequence, PartitionedScheduleError> schedule(SimulationRequest request) {
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const auto index = plan_.worker_for(id);
        if (!index) return std::unexpected{PartitionedScheduleError::UnknownInstrument};
        return workers_.find(*index)->second.schedule(std::move(request));
    }

    [[nodiscard]] StepResult step(std::size_t worker_index) {
        if (worker_index >= plan_.worker_count()) return std::unexpected{PartitionedStepError::InvalidWorker};
        const auto found = workers_.find(worker_index);
        if (found == workers_.end()) return std::nullopt;
        return found->second.step(worker_index);
    }

private:
    WorkerAssignmentPlan plan_;
    // Only active workers need storage; idle indexes are valid even for large plans.
    std::map<std::size_t, detail::SimulationWorker> workers_;
};

} // namespace marketlab
