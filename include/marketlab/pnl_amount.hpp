#pragma once

#include <marketlab/cash_amount.hpp>

namespace marketlab {

struct PnlAmount {
    bool negative{};
    CashAmount magnitude{};

    [[nodiscard]] static constexpr PnlAmount from_difference(CashAmount credits, CashAmount debits) noexcept {
        if (credits >= debits) {
            return {false, *credits.subtract(debits)};
        }
        return {true, *debits.subtract(credits)};
    }

    constexpr bool operator==(const PnlAmount&) const noexcept = default;
};

} // namespace marketlab
