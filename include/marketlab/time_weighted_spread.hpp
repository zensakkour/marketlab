#pragma once

#include <marketlab/aggregate_quantity.hpp>
#include <marketlab/detail/unsigned_product.hpp>
#include <marketlab/time.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace marketlab {

// Exact nonnegative ticks * nanoseconds: high * 2^64 + low.
struct SpreadIntegral {
    std::uint64_t high{};
    std::uint64_t low{};
    constexpr bool operator==(const SpreadIntegral&) const noexcept = default;
};

[[nodiscard]] inline std::string to_string(SpreadIntegral integral) {
    // Reuse the exact two-word decimal formatter; only the numeric words are converted.
    return to_string(AggregateQuantity{integral.high, integral.low});
}

struct MeanSpread {
    SpreadIntegral tick_nanoseconds;
    std::uint64_t covered_nanoseconds;
    constexpr bool operator==(const MeanSpread&) const noexcept = default;
};

struct SpreadStatistics {
    SpreadIntegral tick_nanoseconds{};
    std::uint64_t covered_nanoseconds{};
    std::uint64_t missing_nanoseconds{};

    [[nodiscard]] constexpr std::optional<MeanSpread> mean() const noexcept {
        if (covered_nanoseconds == 0) return std::nullopt;
        return MeanSpread{tick_nanoseconds, covered_nanoseconds};
    }
    constexpr bool operator==(const SpreadStatistics&) const noexcept = default;
};

enum class SpreadObservationError { InvalidTimestamp, InvalidSpread };

// Starts at t=0 with no prices. Observations describe state from their timestamp
// onward; the last observation at an equal timestamp governs the next interval.
class TimeWeightedSpread {
public:
    [[nodiscard]] constexpr std::expected<void, SpreadObservationError> observe(
        LogicalTime time, std::optional<std::int64_t> spread_ticks) noexcept {
        if (time < last_time_) return std::unexpected{SpreadObservationError::InvalidTimestamp};
        if (spread_ticks && *spread_ticks < 0) return std::unexpected{SpreadObservationError::InvalidSpread};
        accumulate(statistics_, time.nanoseconds - last_time_.nanoseconds, last_spread_);
        last_time_ = time;
        last_spread_ = spread_ticks;
        return {};
    }

    // Extend the last state to the caller's horizon without changing this observer.
    [[nodiscard]] constexpr std::expected<SpreadStatistics, SpreadObservationError> snapshot(
        LogicalTime horizon) const noexcept {
        if (horizon < last_time_) return std::unexpected{SpreadObservationError::InvalidTimestamp};
        auto result = statistics_;
        accumulate(result, horizon.nanoseconds - last_time_.nanoseconds, last_spread_);
        return result;
    }

private:
    static constexpr void accumulate(SpreadStatistics& result, std::uint64_t duration,
        std::optional<std::int64_t> spread) noexcept {
        if (!spread) {
            result.missing_nanoseconds += duration;
            return;
        }
        result.covered_nanoseconds += duration;
        const auto product = detail::multiply_unsigned64(static_cast<std::uint64_t>(*spread), duration);
        const auto low = result.tick_nanoseconds.low + product.low;
        const auto carry = static_cast<std::uint64_t>(low < result.tick_nanoseconds.low);
        // Total duration <= UINT64_MAX and spread <= INT64_MAX, so the entire
        // integral stays below 2^127, including this carry.
        result.tick_nanoseconds.high += product.high + carry;
        result.tick_nanoseconds.low = low;
    }

    LogicalTime last_time_{};
    std::optional<std::int64_t> last_spread_;
    SpreadStatistics statistics_;
};

} // namespace marketlab
