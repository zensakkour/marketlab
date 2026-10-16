#pragma once

#include <marketlab/participant_ledger.hpp>

#include <algorithm>
#include <cassert>
#include <deque>
#include <optional>

namespace marketlab {

struct PnlBreakdown {
    PnlAmount realised{};
    PnlAmount unrealised{};
    PnlAmount total{};
    constexpr bool operator==(const PnlBreakdown&) const noexcept = default;
};

// Reconstruct FIFO lots from committed history. Temporary allocation may throw;
// the ledger and all borrowed views remain unchanged.
[[nodiscard]] inline std::optional<PnlBreakdown> fifo_pnl(
    const ParticipantLedger& ledger, ParticipantId participant, Price mark) {
    const auto account = ledger.account(participant);
    if (!account) {
        return std::nullopt;
    }
    if (ledger.history().empty()) {
        return PnlBreakdown{};
    }

    struct Lot {
        Price price;
        Quantity remaining;
    };
    std::deque<Lot> lots;
    bool long_position = false;
    CashAmount realised_credits{};
    CashAmount realised_debits{};
    const auto accumulate = [](CashAmount& total, CashAmount amount) {
        const auto sum = total.add(amount);
        // Execution and marked-open totals are bounded below 2^190.
        assert(sum);
        total = *sum;
    };

    for (const auto& execution : ledger.history()) {
        const bool buying = execution.buyer_id == participant;
        const bool selling = execution.seller_id == participant;
        // Self-matches affect gross accounts, but must not manufacture FIFO realisations.
        if (buying == selling) {
            continue;
        }
        auto remaining = execution.quantity;
        while (remaining.units() != 0 && !lots.empty() && buying != long_position) {
            auto& first = lots.front();
            const auto closed = Quantity::from_units(
                std::min(remaining.units(), first.remaining.units())).value();
            const auto entry = CashAmount::from_trade(first.price, closed);
            const auto exit = CashAmount::from_trade(execution.price, closed);
            accumulate(realised_credits, buying ? entry : exit);
            accumulate(realised_debits, buying ? exit : entry);
            remaining = *remaining.subtract(closed);
            first.remaining = *first.remaining.subtract(closed);
            if (first.remaining.units() == 0) {
                lots.pop_front();
            }
        }
        if (remaining.units() != 0) {
            long_position = buying;
            lots.push_back({execution.price, remaining});
        }
    }

    CashAmount marked{};
    CashAmount basis{};
    for (const auto& lot : lots) {
        // Open quantity is below 2^127; its total marked notional is below 2^190.
        accumulate(marked, CashAmount::from_trade(mark, lot.remaining));
        accumulate(basis, CashAmount::from_trade(lot.price, lot.remaining));
    }
    const auto unrealised = long_position ?
        PnlAmount::from_difference(marked, basis) : PnlAmount::from_difference(basis, marked);
    const auto total = account->total_pnl(mark);
    assert(total); // Every account produced by the ledger fits the exact result type.
    return PnlBreakdown{PnlAmount::from_difference(realised_credits, realised_debits), unrealised, *total};
}

} // namespace marketlab
