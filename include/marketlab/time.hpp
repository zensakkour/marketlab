#pragma once

#include <compare>
#include <cstdint>

namespace marketlab {

struct LogicalTime {
    std::uint64_t nanoseconds{};
    auto operator<=>(const LogicalTime&) const = default;
};

} // namespace marketlab
