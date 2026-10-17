#pragma once

#include <cstdint>
#include <optional>
#include <random>

namespace marketlab {

// Experiment-owned state. Pass it to producers; participants do not seed themselves.
class SimulationRandom {
public:
    explicit SimulationRandom(std::uint64_t seed) : engine_(seed) {}

    // Inclusive bounds. Invalid or singleton ranges consume no random value.
    [[nodiscard]] std::optional<std::uint64_t> uniform(std::uint64_t minimum, std::uint64_t maximum) noexcept {
        if (minimum > maximum) {
            return std::nullopt;
        }
        if (minimum == maximum) {
            return minimum;
        }
        const auto width = maximum - minimum + 1;
        if (width == 0) {
            return engine_(); // Complete uint64 range.
        }
        // Discard the incomplete residue bucket before using modulo.
        const auto threshold = (std::uint64_t{0} - width) % width;
        auto value = engine_();
        while (value < threshold) {
            value = engine_();
        }
        return minimum + value % width;
    }

private:
    std::mt19937_64 engine_;
};

} // namespace marketlab
