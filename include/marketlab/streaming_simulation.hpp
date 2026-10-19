#pragma once

#include <marketlab/bounded_ingress_queue.hpp>
#include <marketlab/partitioned_simulation.hpp>

#include <atomic>
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

enum class StreamingScheduleError { UnknownInstrument, Closed, InvalidTimestamp, SequenceExhausted, Full };
enum class StreamingStepError { InvalidWorker, Empty, Closed, RequestSequenceExhausted, EventSequenceExhausted, OutputCapacityExceeded };

namespace detail {
[[nodiscard]] constexpr StreamingScheduleError streaming_schedule_error(IngressPushError error) {
    switch (error) {
    case IngressPushError::Closed: return StreamingScheduleError::Closed;
    case IngressPushError::InvalidTimestamp: return StreamingScheduleError::InvalidTimestamp;
    case IngressPushError::SequenceExhausted: return StreamingScheduleError::SequenceExhausted;
    case IngressPushError::Full: return StreamingScheduleError::Full;
    }
    throw std::logic_error{"unknown ingress push diagnostic"};
}

[[nodiscard]] constexpr StreamingStepError streaming_step_error(MultiInstrumentError error) {
    switch (error) {
    case MultiInstrumentError::RequestSequenceExhausted: return StreamingStepError::RequestSequenceExhausted;
    case MultiInstrumentError::EventSequenceExhausted: return StreamingStepError::EventSequenceExhausted;
    case MultiInstrumentError::OutputCapacityExceeded: return StreamingStepError::OutputCapacityExceeded;
    case MultiInstrumentError::UnknownInstrument: break;
    }
    throw std::logic_error{"streaming worker received an unowned instrument"};
}
} // namespace detail

// A serialized producer schedules/closes; one exclusive external consumer calls
// step(index) per partition. Observe books and destroy only after consumers join.
class StreamingSimulation {
public:
    using StepResult = std::expected<WorkerStep, StreamingStepError>;

    StreamingSimulation(const InstrumentRegistry& registry, const WorkerAssignmentPlan& plan,
                        std::span<const ParticipantId> participants, std::size_t queue_capacity)
        : plan_{registry, plan.worker_count(), plan.assignments()} {
        if (queue_capacity == 0) throw std::invalid_argument{"streaming capacity must be positive"};
        for (const auto participant : participants) {
            if (!is_valid(participant)) throw std::invalid_argument{"simulation requires nonzero participant IDs"};
        }
        std::map<std::size_t, std::vector<Instrument>> groups;
        for (const auto& instrument : registry.instruments()) {
            groups[*plan_.worker_for(instrument.id)].push_back(instrument);
        }
        for (const auto& [index, instruments] : groups) {
            workers_.try_emplace(index, instruments, participants, queue_capacity);
        }
    }

    StreamingSimulation(const StreamingSimulation&) = delete;
    StreamingSimulation& operator=(const StreamingSimulation&) = delete;

    [[nodiscard]] const WorkerAssignmentPlan& plan() const noexcept { return plan_; }
    [[nodiscard]] bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    // Queue observations synchronize internally; size excludes an in-flight arrival.
    [[nodiscard]] const BoundedIngressQueue* queue(std::size_t worker_index) const noexcept {
        const auto found = workers_.find(worker_index);
        return found == workers_.end() ? nullptr : &found->second.ingress;
    }

    // Addresses are stable; dereference exchange views only after all consumers join.
    [[nodiscard]] const Exchange* find(InstrumentId id) const noexcept {
        const auto index = plan_.worker_for(id);
        return index ? workers_.find(*index)->second.exchange.find(id) : nullptr;
    }

    [[nodiscard]] std::expected<SimulationSequence, StreamingScheduleError> try_schedule(SimulationRequest request) {
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const auto index = plan_.worker_for(id);
        if (!index) return std::unexpected{StreamingScheduleError::UnknownInstrument};
        const auto result = workers_.find(*index)->second.ingress.try_push(std::move(request));
        if (!result) return std::unexpected{detail::streaming_schedule_error(result.error())};
        return *result;
    }

    [[nodiscard]] StepResult step(std::size_t worker_index) {
        if (worker_index >= plan_.worker_count()) return std::unexpected{StreamingStepError::InvalidWorker};
        const auto found = workers_.find(worker_index);
        if (found == workers_.end()) {
            return std::unexpected{is_closed() ? StreamingStepError::Closed : StreamingStepError::Empty};
        }
        auto& worker = found->second;
        if (!worker.pending) {
            auto arrival = worker.ingress.try_pop();
            if (!arrival) {
                return std::unexpected{arrival.error() == IngressPopError::Closed ?
                    StreamingStepError::Closed : StreamingStepError::Empty};
            }
            static_assert(std::is_nothrow_move_constructible_v<SimulationEvent>);
            worker.pending.emplace(std::move(*arrival));
        }
        auto events = std::visit([&](const auto& request) {
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                return worker.exchange.process_order(request);
            } else return worker.exchange.process_cancel(request);
        }, worker.pending->request);
        if (!events) return std::unexpected{detail::streaming_step_error(events.error())};
        // Exchange commit is the last potentially throwing operation. A failed
        // request retains pending; a committed result transfers without allocation.
        static_assert(std::is_nothrow_constructible_v<StepResult, WorkerStep&&>);
        auto arrival = std::move(*worker.pending);
        worker.pending.reset();
        return WorkerStep{worker_index, std::move(arrival), std::move(*events)};
    }

    // Producer-side close is serialized with scheduling. On a mutex error, retry
    // close to finish closing any remaining partitions; none are reopened.
    void close() {
        if (is_closed()) return;
        for (auto& [index, worker] : workers_) {
            static_cast<void>(index);
            worker.ingress.close();
        }
        closed_.store(true, std::memory_order_release);
    }

private:
    struct Worker {
        Worker(std::span<const Instrument> instruments, std::span<const ParticipantId> participants, std::size_t capacity)
            : exchange{InstrumentRegistry{instruments}, participants}, ingress{capacity} {}

        MultiInstrumentExchange exchange;
        BoundedIngressQueue ingress;
        std::optional<SimulationEvent> pending;
    };

    WorkerAssignmentPlan plan_;
    std::map<std::size_t, Worker> workers_;
    std::atomic<bool> closed_{false};
};

} // namespace marketlab
