#pragma once

#include <compare>
#include <cstdint>

namespace marketlab {

struct OrderId {
    std::uint64_t value{};
    auto operator<=>(const OrderId&) const = default;
};

struct ParticipantId {
    std::uint64_t value{};
    auto operator<=>(const ParticipantId&) const = default;
};

struct InstrumentId {
    std::uint64_t value{};
    auto operator<=>(const InstrumentId&) const = default;
};

// Numeric validity does not imply registration, ownership, or order-ID uniqueness.
[[nodiscard]] constexpr bool is_valid(OrderId id) noexcept {
    return id.value != 0;
}

[[nodiscard]] constexpr bool is_valid(ParticipantId id) noexcept {
    return id.value != 0;
}

[[nodiscard]] constexpr bool is_valid(InstrumentId id) noexcept {
    return id.value != 0;
}

} // namespace marketlab
