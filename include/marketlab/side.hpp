#pragma once

#include <cstdint>

namespace marketlab {

enum class Side : std::uint8_t {
    Buy = 1,
    Sell = 2,
};

[[nodiscard]] constexpr bool is_valid(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

} // namespace marketlab
