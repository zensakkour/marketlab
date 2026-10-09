#pragma once

#include <marketlab/events.hpp>
#include <marketlab/execution.hpp>
#include <marketlab/identifiers.hpp>
#include <marketlab/orders.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/requests.hpp>
#include <marketlab/side.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <expected>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

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
    EventSequenceExhausted,
    OutputCapacityExceeded,
};

enum class CancelError {
    InvalidInstrument,
    InvalidParticipant,
    InvalidOrderId,
    InvalidRequestSequence,
    OutOfOrder,
    InvalidEventSequence,
    NotActive,
    NotOwner,
};

struct SingleMatch {
    Execution execution;
    Quantity incoming_remaining;
    Quantity resting_remaining;
};

struct LimitResult {
    std::vector<ExchangeEvent> events;
    Quantity remaining_quantity;
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

    // Active records only; reacquire this borrowed view after any book mutation.
    [[nodiscard]] const RestingOrder* find(OrderId id) const {
        if (!contains(id)) {
            return nullptr;
        }
        for (const auto* levels : {&bids_, &asks_}) {
            for (const auto& level : *levels) {
                const auto position = std::find_if(level.second.begin(), level.second.end(),
                    [id](const RestingOrder& resting) { return resting.order.order_id == id; });
                if (position != level.second.end()) {
                    return &*position;
                }
            }
        }
        return nullptr;
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

        store_passive(resting);
        last_arrival_sequence_ = order.arrival_sequence;
        return PassiveInsertResult::Inserted;
    }

    // Count executions needed against the current book before reserving output.
    // The single writer must keep the book unchanged between counting and matching.
    [[nodiscard]] std::expected<std::size_t, MatchError> match_count(
        const AcceptedOrder& incoming, Quantity remaining) const {
        if (const auto error = validate_match_input(incoming, remaining)) {
            return std::unexpected{*error};
        }
        return preview_limit(incoming, remaining).count;
    }

    // Match one accepted limit remainder. A value contains a match or is empty
    // when no price is eligible; an error leaves the book unchanged.
    [[nodiscard]] std::expected<std::optional<SingleMatch>, MatchError> match_one(
        const AcceptedOrder& incoming, Quantity remaining, EventSequence execution_sequence) {
        if (const auto error = validate_match_input(incoming, remaining)) {
            return std::unexpected{*error};
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
        return match_at(incoming, remaining, execution_sequence, levels, level);
    }

    // Process a trusted accepted limit; exchange registration, clock, and lifetime
    // ID checks belong to the caller. All potentially failing work precedes fills.
    [[nodiscard]] std::expected<LimitResult, MatchError> process_limit(
        const AcceptedOrder& incoming, EventSequence first_event_sequence) {
        if (const auto error = validate_match_input(incoming, incoming.original_quantity)) {
            return std::unexpected{*error};
        }
        if (!is_valid(first_event_sequence)) {
            return std::unexpected{MatchError::InvalidEventSequence};
        }
        const auto preview = preview_limit(incoming, incoming.original_quantity);
        const std::size_t final_events = preview.remaining.units() == 0 ? 1 : 2;
        if (preview.count > (std::numeric_limits<std::size_t>::max() - final_events) / 3) {
            return std::unexpected{MatchError::OutputCapacityExceeded};
        }
        const auto event_count = final_events + 3 * preview.count;
        if (std::cmp_greater(event_count - 1,
                std::numeric_limits<std::uint64_t>::max() - first_event_sequence.value)) {
            return std::unexpected{MatchError::EventSequenceExhausted};
        }
        LimitResult result{{}, preview.remaining};
        if (event_count > result.events.max_size()) {
            return std::unexpected{MatchError::OutputCapacityExceeded};
        }
        result.events.reserve(event_count);
        const auto header = [&] {
            return EventHeader{incoming.accepted_at,
                EventSequence{first_event_sequence.value + result.events.size()},
                incoming.arrival_sequence, instrument_id_};
        };
        result.events.emplace_back(OrderAccepted{header(), incoming});

        // The positive remainder can allocate. Store it before consuming opposite
        // liquidity; no observer or callback can see this temporary crossed state.
        if (preview.remaining.units() != 0) {
            store_passive({incoming, preview.remaining});
        }
        static_assert(std::is_nothrow_copy_constructible_v<ExchangeEvent>);
        static_assert(std::is_nothrow_move_constructible_v<ExchangeEvent>);
        static_assert(std::is_nothrow_constructible_v<std::expected<LimitResult, MatchError>, LimitResult&&>);
        auto remaining = incoming.original_quantity;
        auto& levels = incoming.side == Side::Buy ? asks_ : bids_;
        for (std::size_t index = 0; index < preview.count; ++index) {
            const auto level = incoming.side == Side::Buy ? levels.begin() : std::prev(levels.end());
            const auto match = match_at(incoming, remaining, header().event_sequence, levels, level);
            result.events.emplace_back(match.execution);
            const auto resting_owner = incoming.side == Side::Buy
                ? match.execution.seller_id : match.execution.buyer_id;
            if (match.resting_remaining.units() == 0) {
                result.events.emplace_back(OrderFilled{header(), match.execution.resting_order_id,
                    resting_owner, match.resting_remaining});
            } else {
                result.events.emplace_back(OrderPartiallyFilled{header(), match.execution.resting_order_id,
                    resting_owner, match.resting_remaining});
            }
            remaining = match.incoming_remaining;
            if (remaining.units() == 0) {
                result.events.emplace_back(OrderFilled{header(), incoming.order_id,
                    incoming.participant_id, remaining});
            } else {
                result.events.emplace_back(OrderPartiallyFilled{header(), incoming.order_id,
                    incoming.participant_id, remaining});
            }
        }
        if (preview.remaining.units() != 0) {
            result.events.emplace_back(OrderRested{header(), incoming.order_id,
                incoming.participant_id, *incoming.limit_price, preview.remaining});
        }
        last_arrival_sequence_ = incoming.arrival_sequence;
        return result;
    }

    // Clock/registration validation and counter ownership belong to the exchange.
    // Failures leave the book unchanged; success returns one cancellation record.
    [[nodiscard]] std::expected<OrderCancelled, CancelError> cancel(
        const CancelRequest& request, RequestSequence arrival_sequence, EventSequence event_sequence) {
        if (request.instrument_id != instrument_id_) {
            return std::unexpected{CancelError::InvalidInstrument};
        }
        if (!is_valid(request.participant_id)) {
            return std::unexpected{CancelError::InvalidParticipant};
        }
        if (!is_valid(request.target_order_id)) {
            return std::unexpected{CancelError::InvalidOrderId};
        }
        if (!is_valid(arrival_sequence)) {
            return std::unexpected{CancelError::InvalidRequestSequence};
        }
        if (arrival_sequence <= last_arrival_sequence_) {
            return std::unexpected{CancelError::OutOfOrder};
        }
        if (!is_valid(event_sequence)) {
            return std::unexpected{CancelError::InvalidEventSequence};
        }
        const auto* resting = find(request.target_order_id);
        if (!resting) {
            return std::unexpected{CancelError::NotActive};
        }
        if (resting->order.participant_id != request.participant_id) {
            return std::unexpected{CancelError::NotOwner};
        }
        const OrderCancelled event{
            EventHeader{request.timestamp, event_sequence, arrival_sequence, instrument_id_},
            resting->order.order_id, resting->order.participant_id,
            *resting->order.limit_price, resting->remaining_quantity,
        };
        auto& levels = resting->order.side == Side::Buy ? bids_ : asks_;
        const auto level = levels.find(event.price);
        const auto position = std::find_if(level->second.begin(), level->second.end(),
            [&](const RestingOrder& order) { return order.order.order_id == event.order_id; });
        // Erasing a middle deque element shifts value records. Their assignment
        // and the returned record must not throw after state begins to change.
        static_assert(std::is_nothrow_move_assignable_v<RestingOrder>);
        static_assert(std::is_nothrow_constructible_v<
            std::expected<OrderCancelled, CancelError>, const OrderCancelled&>);
        level->second.erase(position);
        order_ids_.erase(event.order_id);
        if (level->second.empty()) {
            levels.erase(level);
        }
        last_arrival_sequence_ = arrival_sequence;
        return event;
    }

private:
    using PriceLevels = std::map<Price, std::deque<RestingOrder>>;

    struct LimitPreview {
        std::size_t count;
        Quantity remaining;
    };

    [[nodiscard]] LimitPreview preview_limit(const AcceptedOrder& incoming, Quantity remaining) const {
        const bool incoming_buy = incoming.side == Side::Buy;
        const auto& levels = incoming_buy ? asks_ : bids_;
        std::size_t count = 0;
        const auto count_level = [&](const auto& level) {
            if ((incoming_buy && level.first > *incoming.limit_price) ||
                (!incoming_buy && level.first < *incoming.limit_price)) {
                return false;
            }
            for (const auto& resting : level.second) {
                ++count;
                // Subtract fills individually; total available liquidity may overflow.
                const auto quantity = std::min(remaining, resting.remaining_quantity);
                remaining = remaining.subtract(quantity).value();
                if (remaining.units() == 0) {
                    return false;
                }
            }
            return true;
        };
        if (incoming_buy) {
            for (const auto& level : levels) {
                if (!count_level(level)) {
                    break;
                }
            }
        } else {
            for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
                if (!count_level(*level)) {
                    break;
                }
            }
        }
        return {count, remaining};
    }

    // Inputs are validated by the public operation. Insertion retains its strong
    // exception guarantee, including when used to prepare a crossing remainder.
    void store_passive(const RestingOrder& resting) {
        const auto& order = resting.order;
        const auto price = *order.limit_price;
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
    }

    // The caller has established an eligible FIFO head and sufficient event space.
    [[nodiscard]] SingleMatch match_at(const AcceptedOrder& incoming, Quantity remaining,
        EventSequence execution_sequence, PriceLevels& levels, PriceLevels::iterator level) {
        const bool incoming_buy = incoming.side == Side::Buy;
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
                .price = level->first,
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
        return match;
    }

    [[nodiscard]] std::optional<MatchError> validate_match_input(
        const AcceptedOrder& incoming, Quantity remaining) const {
        if (!is_valid(incoming) || incoming.type != OrderType::Limit) {
            return MatchError::InvalidOrder;
        }
        if (!is_valid_order_quantity(remaining) || remaining > incoming.original_quantity) {
            return MatchError::InvalidRemainingQuantity;
        }
        if (incoming.instrument_id != instrument_id_) {
            return MatchError::WrongInstrument;
        }
        if (contains(incoming.order_id)) {
            return MatchError::DuplicateOrderId;
        }
        if (incoming.arrival_sequence <= last_arrival_sequence_) {
            return MatchError::OutOfOrder;
        }
        return std::nullopt;
    }

    InstrumentId instrument_id_;
    PriceLevels bids_;
    PriceLevels asks_;
    std::set<OrderId> order_ids_;
    RequestSequence last_arrival_sequence_{};
};

} // namespace marketlab
