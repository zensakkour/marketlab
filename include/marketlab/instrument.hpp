#pragma once

#include <marketlab/identifiers.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace marketlab {

class TickSize {
public:
    [[nodiscard]] static constexpr std::optional<TickSize> from_decimal(
        std::int64_t coefficient, std::uint32_t scale) noexcept {
        if (coefficient <= 0 || scale > 18) {
            return std::nullopt;
        }
        return TickSize{coefficient, static_cast<std::uint8_t>(scale)};
    }

    [[nodiscard]] constexpr std::int64_t coefficient() const noexcept {
        return coefficient_;
    }

    [[nodiscard]] constexpr std::uint8_t scale() const noexcept {
        return scale_;
    }

private:
    constexpr TickSize(std::int64_t coefficient, std::uint8_t scale) noexcept
        : coefficient_(coefficient), scale_(scale) {}

    std::int64_t coefficient_;
    std::uint8_t scale_;
};

struct Instrument {
    InstrumentId id;
    std::string symbol;
    TickSize tick_size;
};

[[nodiscard]] inline bool is_valid(const Instrument& instrument) noexcept {
    return is_valid(instrument.id) && !instrument.symbol.empty();
}

} // namespace marketlab
