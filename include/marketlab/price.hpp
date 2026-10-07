#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace marketlab {

class Price {
public:
    [[nodiscard]] static constexpr std::optional<Price> from_ticks(std::int64_t ticks) noexcept {
        if (ticks <= 0) {
            return std::nullopt;
        }
        return Price{ticks};
    }

    [[nodiscard]] constexpr std::int64_t ticks() const noexcept {
        return ticks_;
    }

    constexpr auto operator<=>(const Price&) const noexcept = default;

private:
    explicit constexpr Price(std::int64_t ticks) noexcept : ticks_(ticks) {}

    std::int64_t ticks_;
};

} // namespace marketlab
