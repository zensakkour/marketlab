#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace marketlab {

class Quantity {
public:
    [[nodiscard]] static constexpr std::optional<Quantity> from_units(std::int64_t units) noexcept {
        if (units < 0) {
            return std::nullopt;
        }
        return Quantity{units};
    }

    [[nodiscard]] constexpr std::int64_t units() const noexcept {
        return units_;
    }

    [[nodiscard]] constexpr std::optional<Quantity> subtract(Quantity other) const noexcept {
        if (other.units_ > units_) {
            return std::nullopt;
        }
        return Quantity{units_ - other.units_};
    }

    constexpr auto operator<=>(const Quantity&) const noexcept = default;

private:
    explicit constexpr Quantity(std::int64_t units) noexcept : units_(units) {}

    std::int64_t units_;
};

[[nodiscard]] constexpr bool is_valid_order_quantity(Quantity quantity) noexcept {
    return quantity.units() > 0;
}

} // namespace marketlab
