#pragma once

#include <marketlab/events.hpp>
#include <marketlab/execution.hpp>
#include <marketlab/instrument.hpp>
#include <marketlab/order_book.hpp>
#include <marketlab/requests.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace marketlab {

namespace detail {

[[nodiscard]] constexpr std::optional<std::uint64_t> next_sequence_value(std::uint64_t last) noexcept {
    if (last == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return last + 1;
}

} // namespace detail

enum class ExchangeError {
    RequestSequenceExhausted,
    EventSequenceExhausted,
    OutputCapacityExceeded,
};

class Exchange {
public:
    Exchange(Instrument instrument, std::span<const ParticipantId> participants)
        : instrument_(std::move(instrument)), book_(instrument_.id) {
        if (!is_valid(instrument_)) {
            throw std::invalid_argument{"exchange requires a valid instrument"};
        }
        for (const auto participant : participants) {
            if (!is_valid(participant)) {
                throw std::invalid_argument{"exchange requires nonzero participant IDs"};
            }
            participants_.insert(participant);
        }
    }

    [[nodiscard]] const OrderBook& book() const noexcept {
        return book_;
    }

    [[nodiscard]] LogicalTime current_time() const noexcept {
        return current_time_;
    }

    [[nodiscard]] RequestSequence last_request_sequence() const noexcept {
        return last_request_sequence_;
    }

    [[nodiscard]] EventSequence last_event_sequence() const noexcept {
        return last_event_sequence_;
    }

    // Copy the last committed execution; non-trading requests retain it.
    [[nodiscard]] std::optional<Execution> last_trade() const noexcept {
        return last_trade_;
    }

    // Capacity errors and allocation exceptions leave all exchange/book state unchanged.
    [[nodiscard]] std::expected<std::vector<ExchangeEvent>, ExchangeError> process_order(
        const OrderRequest& request) {
        const auto next_request = detail::next_sequence_value(last_request_sequence_.value);
        if (!next_request) {
            return std::unexpected{ExchangeError::RequestSequenceExhausted};
        }
        const auto next_event = detail::next_sequence_value(last_event_sequence_.value);
        if (!next_event) {
            return std::unexpected{ExchangeError::EventSequenceExhausted};
        }
        const RequestSequence request_sequence{*next_request};
        const EventSequence first_event_sequence{*next_event};
        const auto event_time = std::max(current_time_, request.timestamp);
        static_assert(std::is_nothrow_constructible_v<
            std::expected<std::vector<ExchangeEvent>, ExchangeError>, std::vector<ExchangeEvent>&&>);
        if (const auto reason = rejection_reason(request)) {
            std::vector<ExchangeEvent> events{OrderRejected{
                {event_time, first_event_sequence, request_sequence, request.instrument_id}, request, *reason}};
            commit_request(event_time, request_sequence, first_event_sequence);
            return events;
        }

        const AcceptedOrder accepted{request.order_id, request.participant_id, request.instrument_id,
            request.side, request.type, Quantity::from_units(request.quantity_units).value(),
            request.limit_price_ticks ? Price::from_ticks(*request.limit_price_ticks) : std::nullopt,
            request.timestamp, request_sequence};
        // Reserve lifetime identity before matching; rollback if book preparation fails.
        const auto identity = accepted_order_ids_.insert(request.order_id).first;
        try {
            auto result = book_.process_order(accepted, first_event_sequence);
            if (!result) {
                switch (result.error()) {
                case MatchError::EventSequenceExhausted:
                    accepted_order_ids_.erase(identity);
                    return std::unexpected{ExchangeError::EventSequenceExhausted};
                case MatchError::OutputCapacityExceeded:
                    accepted_order_ids_.erase(identity);
                    return std::unexpected{ExchangeError::OutputCapacityExceeded};
                default:
                    throw std::logic_error{"validated exchange order failed book input checks"};
                }
            }
            const EventSequence last_event{first_event_sequence.value +
                static_cast<std::uint64_t>(result->events.size() - 1)};
            // Book processing has committed; only non-throwing value copies remain.
            static_assert(std::is_nothrow_assignable_v<std::optional<Execution>&, const Execution&>);
            for (auto event = result->events.rbegin(); event != result->events.rend(); ++event) {
                if (const auto* execution = std::get_if<Execution>(&*event)) {
                    last_trade_ = *execution;
                    break;
                }
            }
            commit_request(event_time, request_sequence, last_event);
            return std::move(result->events);
        } catch (...) {
            accepted_order_ids_.erase(identity);
            throw;
        }
    }

    [[nodiscard]] std::expected<std::vector<ExchangeEvent>, ExchangeError> process_cancel(
        const CancelRequest& request) {
        const auto next_request = detail::next_sequence_value(last_request_sequence_.value);
        if (!next_request) {
            return std::unexpected{ExchangeError::RequestSequenceExhausted};
        }
        const auto next_event = detail::next_sequence_value(last_event_sequence_.value);
        if (!next_event) {
            return std::unexpected{ExchangeError::EventSequenceExhausted};
        }
        const RequestSequence request_sequence{*next_request};
        const EventSequence event_sequence{*next_event};
        const auto event_time = std::max(current_time_, request.timestamp);
        auto reason = rejection_reason(request);
        std::vector<ExchangeEvent> events;
        // Reserve output before cancellation; the book removal itself allocates nothing.
        events.reserve(1);
        static_assert(std::is_nothrow_constructible_v<ExchangeEvent, const OrderCancelled&>);
        static_assert(std::is_nothrow_constructible_v<
            std::expected<std::vector<ExchangeEvent>, ExchangeError>, std::vector<ExchangeEvent>&&>);
        if (!reason) {
            const auto result = book_.cancel(request, request_sequence, event_sequence);
            if (result) {
                events.emplace_back(*result);
            } else {
                switch (result.error()) {
                case CancelError::NotActive: reason = RejectionReason::NotActive; break;
                case CancelError::NotOwner: reason = RejectionReason::NotOwner; break;
                default:
                    throw std::logic_error{"validated exchange cancellation failed book input checks"};
                }
            }
        }
        if (reason) {
            events.emplace_back(CancelRejected{
                {event_time, event_sequence, request_sequence, request.instrument_id}, request, *reason});
        }
        commit_request(event_time, request_sequence, event_sequence);
        return events;
    }

private:
    [[nodiscard]] std::optional<RejectionReason> rejection_reason(const OrderRequest& request) const {
        if (request.timestamp < current_time_) {
            return RejectionReason::InvalidTimestamp;
        }
        if (request.instrument_id != instrument_.id) {
            return RejectionReason::InvalidInstrument;
        }
        if (!participants_.contains(request.participant_id)) {
            return RejectionReason::InvalidParticipant;
        }
        if (const auto reason = validate_fields(request)) {
            return reason;
        }
        if (accepted_order_ids_.contains(request.order_id)) {
            return RejectionReason::DuplicateOrderId;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<RejectionReason> rejection_reason(const CancelRequest& request) const {
        if (request.timestamp < current_time_) {
            return RejectionReason::InvalidTimestamp;
        }
        if (request.instrument_id != instrument_.id) {
            return RejectionReason::InvalidInstrument;
        }
        if (!participants_.contains(request.participant_id)) {
            return RejectionReason::InvalidParticipant;
        }
        return validate_fields(request);
    }

    void commit_request(LogicalTime timestamp, RequestSequence request_sequence,
        EventSequence last_event_sequence) noexcept {
        current_time_ = timestamp;
        last_request_sequence_ = request_sequence;
        last_event_sequence_ = last_event_sequence;
    }

    Instrument instrument_;
    std::set<ParticipantId> participants_;
    OrderBook book_;
    std::set<OrderId> accepted_order_ids_;
    LogicalTime current_time_{};
    RequestSequence last_request_sequence_{};
    EventSequence last_event_sequence_{};
    std::optional<Execution> last_trade_;
};

} // namespace marketlab
