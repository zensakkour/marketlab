#pragma once

#include <marketlab/identifiers.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace marketlab {

// Experiment-owned client IDs; these do not assign exchange request/event sequences.
class OrderIdSource {
public:
    explicit constexpr OrderIdSource(OrderId first = {1}) : next_(first) {
        if (!is_valid(first)) {
            throw std::invalid_argument{"order IDs must start above zero"};
        }
    }

    [[nodiscard]] constexpr std::optional<OrderId> peek() const noexcept {
        return exhausted_ ? std::nullopt : std::optional{next_};
    }

    [[nodiscard]] constexpr std::optional<OrderId> next() noexcept {
        if (exhausted_) {
            return std::nullopt;
        }
        const auto id = next_;
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            exhausted_ = true;
        } else {
            ++next_.value;
        }
        return id;
    }

private:
    OrderId next_;
    bool exhausted_{};
};

} // namespace marketlab
