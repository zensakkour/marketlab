#pragma once

#include <marketlab/simulation.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

namespace marketlab {

class ScriptedParticipant {
public:
    ScriptedParticipant(ParticipantId participant, std::span<const SimulationRequest> script)
        : participant_(participant) {
        if (!is_valid(participant)) {
            throw std::invalid_argument{"script requires a nonzero participant ID"};
        }
        std::optional<LogicalTime> previous;
        for (const auto& action : script) {
            const auto owner = std::visit([](const auto& request) { return request.participant_id; }, action);
            const auto time = std::visit([](const auto& request) { return request.timestamp; }, action);
            if (owner != participant || (previous && time < *previous)) {
                throw std::invalid_argument{"script requires one owner and nondecreasing times"};
            }
            previous = time;
        }
        actions_.assign(script.begin(), script.end());
    }

    ScriptedParticipant(const ScriptedParticipant&) = delete;
    ScriptedParticipant& operator=(const ScriptedParticipant&) = delete;

    [[nodiscard]] ParticipantId participant_id() const noexcept { return participant_; }
    [[nodiscard]] std::size_t submitted_count() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return actions_.size() - cursor_; }
    [[nodiscard]] bool empty() const noexcept { return cursor_ == actions_.size(); }

    // Stored actions never mutate; borrowed records remain valid until destruction.
    [[nodiscard]] const SimulationRequest* next() const noexcept {
        return empty() ? nullptr : &actions_[cursor_];
    }

    [[nodiscard]] std::expected<std::optional<SimulationSequence>, SimulationScheduleError>
    schedule_next(Simulation& simulation) {
        if (empty()) {
            return std::nullopt;
        }
        const auto result = simulation.schedule(actions_[cursor_]);
        if (!result) {
            return std::unexpected{result.error()};
        }
        static_assert(std::is_nothrow_constructible_v<
            std::expected<std::optional<SimulationSequence>, SimulationScheduleError>, SimulationSequence>);
        ++cursor_;
        return *result;
    }

private:
    ParticipantId participant_;
    std::vector<SimulationRequest> actions_;
    std::size_t cursor_{};
};

} // namespace marketlab
