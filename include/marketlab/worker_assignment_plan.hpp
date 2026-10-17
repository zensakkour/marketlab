#pragma once

#include <marketlab/instrument_registry.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace marketlab {

struct InstrumentWorkerAssignment {
    InstrumentId instrument_id;
    std::size_t worker_index;

    constexpr bool operator==(const InstrumentWorkerAssignment&) const noexcept = default;
};

// Fixed configuration for future worker ownership; this plan creates no workers.
class WorkerAssignmentPlan {
public:
    WorkerAssignmentPlan(const InstrumentRegistry& registry, std::size_t worker_count,
                         std::span<const InstrumentWorkerAssignment> assignments)
        : worker_count_{worker_count} {
        if (worker_count == 0) throw std::invalid_argument{"worker count must be positive"};
        if (assignments.size() != registry.size()) {
            throw std::invalid_argument{"worker plan requires one assignment per instrument"};
        }
        for (const auto& assignment : assignments) {
            if (!registry.find(assignment.instrument_id)) {
                throw std::invalid_argument{"worker plan requires registered instruments"};
            }
            if (assignment.worker_index >= worker_count) {
                throw std::invalid_argument{"worker index is out of range"};
            }
        }
        assignments_.assign(assignments.begin(), assignments.end());
        std::ranges::sort(assignments_, {}, &InstrumentWorkerAssignment::instrument_id);
        const auto duplicate = std::adjacent_find(assignments_.begin(), assignments_.end(),
            [](const auto& left, const auto& right) { return left.instrument_id == right.instrument_id; });
        if (duplicate != assignments_.end()) {
            throw std::invalid_argument{"worker plan requires unique instrument assignments"};
        }
    }

    WorkerAssignmentPlan(const WorkerAssignmentPlan&) = delete;
    WorkerAssignmentPlan& operator=(const WorkerAssignmentPlan&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept { return worker_count_; }

    // Borrowed assignments stay valid until plan destruction.
    [[nodiscard]] std::span<const InstrumentWorkerAssignment> assignments() const noexcept { return assignments_; }

    [[nodiscard]] std::optional<std::size_t> worker_for(InstrumentId id) const noexcept {
        const auto found = std::ranges::lower_bound(assignments_, id, {}, &InstrumentWorkerAssignment::instrument_id);
        if (found == assignments_.end() || found->instrument_id != id) return std::nullopt;
        return found->worker_index;
    }

private:
    std::size_t worker_count_;
    std::vector<InstrumentWorkerAssignment> assignments_;
};

} // namespace marketlab
