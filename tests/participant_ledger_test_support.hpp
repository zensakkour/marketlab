#pragma once

#include <marketlab/participant_ledger.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ledger_test {

using namespace marketlab;
inline constexpr std::array participants{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}};

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

inline Execution trade(std::uint64_t event, std::uint64_t request, std::uint64_t time,
    std::int64_t units, std::uint64_t buyer = 1, std::uint64_t seller = 2) {
    return {{time}, {event}, {request}, {1}, {1}, {2}, Side::Buy, {buyer}, {seller},
        Price::from_ticks(100).value(), Quantity::from_units(units).value()};
}

inline ParticipantAccount account_at_100(std::uint64_t bought, std::uint64_t sold) {
    // Small fixture quantities only; boundary expectations are specified explicitly.
    return {{0, bought}, {0, sold}, {0, 0, 100 * bought}, {0, 0, 100 * sold}};
}

inline bool same_execution(const Execution& left, const Execution& right) {
    return left.timestamp == right.timestamp && left.event_sequence == right.event_sequence &&
        left.request_sequence == right.request_sequence && left.instrument_id == right.instrument_id &&
        left.aggressive_order_id == right.aggressive_order_id && left.resting_order_id == right.resting_order_id &&
        left.aggressive_side == right.aggressive_side && left.buyer_id == right.buyer_id && left.seller_id == right.seller_id &&
        left.price == right.price && left.quantity == right.quantity;
}

} // namespace ledger_test
