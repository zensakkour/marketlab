#pragma once

#include <marketlab/aggregate_quantity.hpp>
#include <marketlab/cash_amount.hpp>
#include <marketlab/execution.hpp>
#include <marketlab/identifiers.hpp>
#include <marketlab/pnl_amount.hpp>

#include <cassert>
#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace marketlab {

struct NetPosition {
    bool negative{};
    AggregateQuantity magnitude{};
    constexpr bool operator==(const NetPosition&) const noexcept = default;
};

using TotalPnl = PnlAmount;

struct ParticipantAccount {
    AggregateQuantity buy_volume{};
    AggregateQuantity sell_volume{};
    CashAmount buy_notional{};
    CashAmount sell_notional{};

    [[nodiscard]] constexpr NetPosition position() const noexcept {
        if (buy_volume >= sell_volume) {
            return {false, *buy_volume.subtract(sell_volume)};
        }
        return {true, *sell_volume.subtract(buy_volume)};
    }

    [[nodiscard]] constexpr NetCashFlow cash_flow() const noexcept {
        if (sell_notional >= buy_notional) {
            return {false, *sell_notional.subtract(buy_notional)};
        }
        return {true, *buy_notional.subtract(sell_notional)};
    }

    [[nodiscard]] constexpr std::optional<TotalPnl> total_pnl(Price mark) const noexcept {
        const auto cash = cash_flow();
        const auto net = position();
        const auto inventory = CashAmount::from_aggregate(mark, net.magnitude);
        if (cash.negative == net.negative) {
            const auto sum = cash.magnitude.add(inventory);
            if (!sum) {
                return std::nullopt;
            }
            return TotalPnl{cash.negative && *sum != CashAmount{}, *sum};
        }
        if (cash.magnitude >= inventory) {
            const auto difference = *cash.magnitude.subtract(inventory);
            return TotalPnl{cash.negative && difference != CashAmount{}, difference};
        }
        return TotalPnl{net.negative, *inventory.subtract(cash.magnitude)};
    }

    constexpr bool operator==(const ParticipantAccount&) const noexcept = default;
};

enum class AccountingError {
    InvalidExecution,
    WrongInstrument,
    UnknownParticipant,
    OutOfOrder,
    HistoryCapacityExceeded,
};

class ParticipantLedger {
public:
    ParticipantLedger(InstrumentId instrument_id, std::span<const ParticipantId> participants)
        : instrument_id_(instrument_id) {
        if (!is_valid(instrument_id)) {
            throw std::invalid_argument{"ledger requires a nonzero instrument ID"};
        }
        for (const auto participant : participants) {
            if (!is_valid(participant)) {
                throw std::invalid_argument{"ledger requires nonzero participant IDs"};
            }
            accounts_.try_emplace(participant);
        }
    }

    ParticipantLedger(const ParticipantLedger&) = delete;
    ParticipantLedger& operator=(const ParticipantLedger&) = delete;

    [[nodiscard]] InstrumentId instrument_id() const noexcept {
        return instrument_id_;
    }

    [[nodiscard]] std::optional<ParticipantAccount> account(ParticipantId participant) const noexcept {
        const auto found = accounts_.find(participant);
        if (found == accounts_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    [[nodiscard]] EventSequence last_event_sequence() const noexcept {
        return history_.empty() ? EventSequence{} : history_.back().event_sequence;
    }

    // Borrowed history; reacquire after successful nonempty ingestion.
    [[nodiscard]] std::span<const Execution> history() const noexcept {
        return history_;
    }

    // One complete execution batch or no mutation, including allocation failure.
    // Matching authenticity belongs to the exchange that supplies these records.
    [[nodiscard]] std::expected<void, AccountingError> apply(std::span<const Execution> executions) {
        const Execution* previous = history_.empty() ? nullptr : &history_.back();
        for (const auto& execution : executions) {
            if (!is_valid(execution)) {
                return std::unexpected{AccountingError::InvalidExecution};
            }
            if (execution.instrument_id != instrument_id_) {
                return std::unexpected{AccountingError::WrongInstrument};
            }
            if (!accounts_.contains(execution.buyer_id) || !accounts_.contains(execution.seller_id)) {
                return std::unexpected{AccountingError::UnknownParticipant};
            }
            if (previous && (execution.event_sequence <= previous->event_sequence ||
                    execution.timestamp < previous->timestamp || execution.request_sequence < previous->request_sequence ||
                    (execution.request_sequence == previous->request_sequence && execution.timestamp != previous->timestamp))) {
                return std::unexpected{AccountingError::OutOfOrder};
            }
            previous = &execution;
        }
        if (executions.empty()) {
            return {};
        }
        if (executions.size() > history_.max_size() - history_.size()) {
            return std::unexpected{AccountingError::HistoryCapacityExceeded};
        }

        // Prepare every participant update before extending history, so a failed
        // preparation also preserves all existing borrowed history views.
        auto prepared = accounts_;
        for (const auto& execution : executions) {
            auto& buyer = prepared.find(execution.buyer_id)->second;
            auto& seller = prepared.find(execution.seller_id)->second;
            const auto bought = buyer.buy_volume.add(execution.quantity);
            const auto sold = seller.sell_volume.add(execution.quantity);
            // Strict uint64 event ordering bounds execution count; each size is int64.
            // Even arbitrary valid input therefore keeps each total below 2^127.
            assert(bought && sold);
            buyer.buy_volume = *bought;
            seller.sell_volume = *sold;
            const auto notional = CashAmount::from_trade(execution.price, execution.quantity);
            const auto spent = buyer.buy_notional.add(notional);
            const auto received = seller.sell_notional.add(notional);
            // At most UINT64_MAX executions, each notional below 2^126: sum < 2^190.
            assert(spent && received);
            buyer.buy_notional = *spent;
            seller.sell_notional = *received;
        }
        static_assert(std::is_nothrow_copy_constructible_v<Execution>);
        static_assert(std::is_nothrow_move_constructible_v<Execution>);
        static_assert(noexcept(accounts_.swap(prepared)));
        // End insertion has the strong exception guarantee for these nonthrowing
        // value records. After it succeeds, swapping prepared accounts cannot fail.
        history_.insert(history_.end(), executions.begin(), executions.end());
        accounts_.swap(prepared);
        return {};
    }

private:
    InstrumentId instrument_id_;
    std::map<ParticipantId, ParticipantAccount> accounts_;
    std::vector<Execution> history_;
};

} // namespace marketlab
