#pragma once

#include <marketlab/participant_ledger.hpp>

#include <cstdint>
#include <optional>

namespace marketlab {

struct ParticipantFills {
    std::uint64_t buy_count{};
    std::uint64_t sell_count{};
    std::uint64_t aggressive_count{};
    std::uint64_t passive_count{};
    constexpr bool operator==(const ParticipantFills&) const noexcept = default;
};

// One fill per counterparty leg of a committed execution, independent of quantity.
// Self-matches contribute one buy, one sell, one aggressive and one passive fill.
[[nodiscard]] inline std::optional<ParticipantFills> participant_fills(
    const ParticipantLedger& ledger, ParticipantId participant) noexcept {
    if (!ledger.account(participant)) {
        return std::nullopt;
    }
    ParticipantFills result;
    for (const auto& execution : ledger.history()) {
        // Each directional/role counter increases at most once per execution.
        // Strict positive uint64 event ordering bounds each count by UINT64_MAX.
        if (execution.buyer_id == participant) {
            ++result.buy_count;
            if (execution.aggressive_side == Side::Buy) ++result.aggressive_count;
            else ++result.passive_count;
        }
        if (execution.seller_id == participant) {
            ++result.sell_count;
            if (execution.aggressive_side == Side::Sell) ++result.aggressive_count;
            else ++result.passive_count;
        }
    }
    return result;
}

} // namespace marketlab
