#pragma once

#include <marketlab/execution.hpp>
#include <marketlab/identifiers.hpp>
#include <marketlab/orders.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/side.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <expected>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace marketlab {

enum class PassiveInsertResult {
    Inserted,
    InvalidOrder,
    WrongInstrument,
    DuplicateOrderId,
    OutOfOrder,
    WouldCross,
};

enum class MatchError {
    InvalidOrder,
    InvalidRemainingQuantity,
    WrongInstrument,
    DuplicateOrderId,
    OutOfOrder,
    InvalidEventSequence,
};

struct SingleMatch {
    Execution execution;
    Quantity incoming_remaining;
    Quantity resting_remaining;
};

class OrderBook {
public:
    explicit OrderBook(InstrumentId instrument_id) : instrument_id_(instrument_id) {
        if (!is_valid(instrument_id)) {
            throw std::invalid_argument{"order book requires a nonzero instrument ID"};
        }
    }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    [[nodiscard]] InstrumentId instrument_id() const noexcept {
        return instrument_id_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return order_ids_.empty();
    }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return order_ids_.size();
    }

    [[nodiscard]] bool contains(OrderId id) const {
        return order_ids_.contains(id);
    }

    // Borrowed read-only records: callers must reacquire them after a mutation.
    [[nodiscard]] const RestingOrder* best_bid() const noexcept {
        return bids_.empty() ? nullptr : &bids_.rbegin()->second.front();
    }

    [[nodiscard]] const RestingOrder* best_ask() const noexcept {
        return asks_.empty() ? nullptr : &asks_.begin()->second.front();
    }

    // This storage operation does not accept a request or emit exchange events.
    // A future exchange supplies accepted remainders in increasing arrival order.
    [[nodiscard]] PassiveInsertResult insert_passive(const RestingOrder& resting) {
        if (!is_valid(resting)) {
            return PassiveInsertResult::InvalidOrder;
        }
        const auto& order = resting.order;
        if (order.instrument_id != instrument_id_) {
            return PassiveInsertResult::WrongInstrument;
        }
        if (contains(order.order_id)) {
            return PassiveInsertResult::DuplicateOrderId;
        }
        if (order.arrival_sequence <= last_arrival_sequence_) {
            return PassiveInsertResult::OutOfOrder;
        }
        const auto price = *order.limit_price;
        if ((order.side == Side::Buy && !asks_.empty() && price >= asks_.begin()->first) ||
            (order.side == Side::Sell && !bids_.empty() && price <= bids_.rbegin()->first)) {
            return PassiveInsertResult::WouldCross;
        }

        auto& levels = order.side == Side::Buy ? bids_ : asks_;
        const auto id_position = order_ids_.insert(order.order_id).first;
        try {
            const auto [level, created] = levels.try_emplace(price);
            try {
                level->second.push_back(resting);
            } catch (...) {
                if (created) {
                    levels.erase(level);
                }
                throw;
            }
        } catch (...) {
            // Allocation failure must not leave a phantom active ID or empty level.
            order_ids_.erase(id_position);
            throw;
        }
        last_arrival_sequence_ = order.arrival_sequence;
        return PassiveInsertResult::Inserted;
    }

    // Match one accepted limit remainder. A value contains a match or is empty
    // when no price is eligible; an error leaves the book unchanged.
    [[nodiscard]] std::expected<std::optional<SingleMatch>, MatchError> match_one(
        const AcceptedOrder& incoming, Quantity remaining, EventSequence execution_sequence) {
        if (!is_valid(incoming) || incoming.type != OrderType::Limit) {
            return std::unexpected{MatchError::InvalidOrder};
        }
        if (!is_valid_order_quantity(remaining) || remaining > incoming.original_quantity) {
            return std::unexpected{MatchError::InvalidRemainingQuantity};
        }
        if (incoming.instrument_id != instrument_id_) {
            return std::unexpected{MatchError::WrongInstrument};
        }
        if (contains(incoming.order_id)) {
            return std::unexpected{MatchError::DuplicateOrderId};
        }
        if (incoming.arrival_sequence <= last_arrival_sequence_) {
            return std::unexpected{MatchError::OutOfOrder};
        }
        if (!is_valid(execution_sequence)) {
            return std::unexpected{MatchError::InvalidEventSequence};
        }

        const bool incoming_buy = incoming.side == Side::Buy;
        auto& levels = incoming_buy ? asks_ : bids_;
        if (levels.empty()) {
            return std::nullopt;
        }
        const auto level = incoming_buy ? levels.begin() : std::prev(levels.end());
        const auto price = level->first;
        if ((incoming_buy && price > *incoming.limit_price) ||
            (!incoming_buy && price < *incoming.limit_price)) {
            return std::nullopt;
        }
        auto& resting = level->second.front();
        const auto quantity = std::min(remaining, resting.remaining_quantity);
        // The minimum bounds both subtractions before any state changes.
        const auto incoming_remaining = remaining.subtract(quantity).value();
        const auto resting_remaining = resting.remaining_quantity.subtract(quantity).value();
        const SingleMatch match{
            Execution{
                .timestamp = incoming.accepted_at,
                .event_sequence = execution_sequence,
                .request_sequence = incoming.arrival_sequence,
                .instrument_id = instrument_id_,
                .aggressive_order_id = incoming.order_id,
                .resting_order_id = resting.order.order_id,
                .aggressive_side = incoming.side,
                .buyer_id = incoming_buy ? incoming.participant_id : resting.order.participant_id,
                .seller_id = incoming_buy ? resting.order.participant_id : incoming.participant_id,
                .price = price,
                .quantity = quantity,
            },
            incoming_remaining,
            resting_remaining,
        };

        if (resting_remaining.units() == 0) {
            order_ids_.erase(resting.order.order_id);
            level->second.pop_front();
            if (level->second.empty()) {
                levels.erase(level);
            }
        } else {
            resting.remaining_quantity = resting_remaining;
        }
        return std::optional{match};
    }

private:
    using PriceLevels = std::map<Price, std::deque<RestingOrder>>;

    InstrumentId instrument_id_;
    PriceLevels bids_;
    PriceLevels asks_;
    std::set<OrderId> order_ids_;
    RequestSequence last_arrival_sequence_{};
};

} // namespace marketlab
