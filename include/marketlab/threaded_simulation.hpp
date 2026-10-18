#pragma once

#include <marketlab/partitioned_simulation.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <latch>
#include <map>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace marketlab {

struct WorkerBatch {
    std::size_t worker_index;
    std::vector<WorkerStep> steps;
    std::optional<PartitionedStepError> error;
    std::exception_ptr exception;

    [[nodiscard]] bool succeeded() const noexcept { return !error && !exception; }
};

// Closed batches: the caller schedules/observes between run calls only. Each
// nonempty partition has exactly one writer thread until run has joined them all.
class ThreadedSimulation {
public:
    ThreadedSimulation(const InstrumentRegistry& registry, const WorkerAssignmentPlan& plan,
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

    ThreadedSimulation(const ThreadedSimulation&) = delete;
    ThreadedSimulation& operator=(const ThreadedSimulation&) = delete;

    [[nodiscard]] const WorkerAssignmentPlan& plan() const noexcept { return plan_; }

    // Borrowed addresses stay valid until destruction; do not read through them during run().
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

    [[nodiscard]] std::vector<WorkerBatch> run() {
        std::size_t count = 0;
        for (const auto& [index, worker] : workers_) {
            static_cast<void>(index);
            if (!worker.queue.empty()) ++count;
        }
        std::vector<WorkerBatch> results;
        results.reserve(count);
        for (const auto& [index, worker] : workers_) {
            if (worker.queue.empty()) continue;
            results.push_back(WorkerBatch{index, {}, {}, {}});
            // Reserve every publication slot before any thread can commit an arrival.
            results.back().steps.reserve(worker.queue.size());
        }
        if (results.empty()) return results;

        std::latch start{1};
        // Explicit release/acquire publication also makes the startup decision
        // visible to race detectors independently of latch instrumentation.
        std::atomic<bool> proceed{false};
        {
            std::vector<std::jthread> threads;
            threads.reserve(results.size());
            try {
                for (auto& result : results) {
                    auto* worker = &workers_.find(result.worker_index)->second;
                    threads.emplace_back([&start, &proceed, worker, output = &result]() noexcept {
                        try {
                            start.wait();
                            if (!proceed.load(std::memory_order_acquire)) return;
                            while (!worker->queue.empty()) {
                                auto step = worker->step(output->worker_index);
                                if (!step) {
                                    output->error = step.error();
                                    return;
                                }
                                // Closed input bounds step count by the capacity reserved above.
                                static_assert(std::is_nothrow_move_constructible_v<WorkerStep>);
                                output->steps.push_back(std::move(**step));
                            }
                        } catch (...) {
                            output->exception = std::current_exception();
                        }
                    });
                }
            } catch (...) {
                // Already-created threads leave without touching books/queues, then join.
                start.count_down();
                throw;
            }
            proceed.store(true, std::memory_order_release);
            start.count_down();
        } // jthread destruction joins every writer before results or views become observable.
        return results;
    }

private:
    WorkerAssignmentPlan plan_;
    std::map<std::size_t, detail::SimulationWorker> workers_;
};

} // namespace marketlab
