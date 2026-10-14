#include <marketlab/exchange.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

using marketlab::AcceptedOrder;
using marketlab::CancelRejected;
using marketlab::CancelRequest;
using marketlab::EventHeader;
using marketlab::Exchange;
using marketlab::ExchangeEvent;
using marketlab::Execution;
using marketlab::Instrument;
using marketlab::MarketRemainderExpired;
using marketlab::OrderAccepted;
using marketlab::OrderCancelled;
using marketlab::OrderFilled;
using marketlab::OrderId;
using marketlab::OrderPartiallyFilled;
using marketlab::OrderRejected;
using marketlab::OrderRequest;
using marketlab::OrderRested;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::Quantity;
using marketlab::RejectionReason;
using marketlab::Side;
using marketlab::TickSize;

using Command = std::variant<OrderRequest, CancelRequest>;
constexpr auto maximum_quantity = std::numeric_limits<std::int64_t>::max();

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

bool same_request(const OrderRequest& left, const OrderRequest& right) {
    return left.timestamp == right.timestamp && left.instrument_id == right.instrument_id &&
        left.participant_id == right.participant_id && left.order_id == right.order_id && left.side == right.side &&
        left.type == right.type && left.quantity_units == right.quantity_units && left.limit_price_ticks == right.limit_price_ticks;
}

bool same_request(const CancelRequest& left, const CancelRequest& right) {
    return left.timestamp == right.timestamp && left.instrument_id == right.instrument_id &&
        left.participant_id == right.participant_id && left.target_order_id == right.target_order_id;
}

bool same_command(const Command& left, const Command& right) {
    return std::visit([&](const auto& request) {
        const auto* other = std::get_if<std::decay_t<decltype(request)>>(&right);
        return other && same_request(request, *other);
    }, left);
}

struct TrackedOrder {
    OrderRequest request;
    std::int64_t remaining;
    std::uint64_t priority;
};

struct Ledger {
    std::map<OrderId, TrackedOrder> active;
    std::set<OrderId> accepted;
    std::vector<OrderId> accepted_ids;
    std::uint64_t time{};
    std::uint64_t events{};
    std::array<std::size_t, 6> categories{};

    // A flat scan supplies an independent price/FIFO reference for the indexed book.
    auto best(Side side) const {
        auto result = active.end();
        for (auto candidate = active.begin(); candidate != active.end(); ++candidate) {
            const auto& order = candidate->second;
            if (order.request.side != side) continue;
            if (result == active.end()) {
                result = candidate;
                continue;
            }
            const auto price = *order.request.limit_price_ticks;
            const auto prior_price = *result->second.request.limit_price_ticks;
            if ((side == Side::Buy ? price > prior_price : price < prior_price) ||
                (price == prior_price && order.priority < result->second.priority)) {
                result = candidate;
            }
        }
        return result;
    }
};

bool eligible(const Ledger& ledger, const OrderRequest& incoming) {
    const auto opposite = ledger.best(incoming.side == Side::Buy ? Side::Sell : Side::Buy);
    if (opposite == ledger.active.end()) return false;
    if (incoming.type == OrderType::Market) return true;
    const auto price = *opposite->second.request.limit_price_ticks;
    return incoming.side == Side::Buy ? price <= *incoming.limit_price_ticks : price >= *incoming.limit_price_ticks;
}

std::optional<RejectionReason> registration_reason(std::uint64_t time, marketlab::InstrumentId instrument,
    ParticipantId participant, const Ledger& ledger) {
    if (time < ledger.time) return RejectionReason::InvalidTimestamp;
    if (instrument.value != 1) return RejectionReason::InvalidInstrument;
    if (participant.value < 1 || participant.value > 3) return RejectionReason::InvalidParticipant;
    return std::nullopt;
}

std::optional<RejectionReason> rejection_reason(const OrderRequest& request, const Ledger& ledger) {
    if (const auto reason = registration_reason(request.timestamp.nanoseconds, request.instrument_id,
            request.participant_id, ledger)) return reason;
    if (request.order_id.value == 0) return RejectionReason::InvalidOrderId;
    if (request.side != Side::Buy && request.side != Side::Sell) return RejectionReason::InvalidSide;
    if (request.type != OrderType::Limit && request.type != OrderType::Market) return RejectionReason::InvalidOrderType;
    if (request.quantity_units <= 0) return RejectionReason::InvalidQuantity;
    if ((request.type == OrderType::Limit && (!request.limit_price_ticks || *request.limit_price_ticks <= 0)) ||
        (request.type == OrderType::Market && request.limit_price_ticks)) return RejectionReason::InvalidPrice;
    if (ledger.accepted.contains(request.order_id)) return RejectionReason::DuplicateOrderId;
    return std::nullopt;
}

std::optional<RejectionReason> rejection_reason(const CancelRequest& request, const Ledger& ledger) {
    if (const auto reason = registration_reason(request.timestamp.nanoseconds, request.instrument_id,
            request.participant_id, ledger)) return reason;
    if (request.target_order_id.value == 0) return RejectionReason::InvalidOrderId;
    const auto target = ledger.active.find(request.target_order_id);
    if (target == ledger.active.end()) return RejectionReason::NotActive;
    if (target->second.request.participant_id != request.participant_id) return RejectionReason::NotOwner;
    return std::nullopt;
}

EventHeader event_header(const ExchangeEvent& event) {
    return std::visit([](const auto& record) {
        if constexpr (std::is_same_v<std::decay_t<decltype(record)>, Execution>) {
            return EventHeader{record.timestamp, record.event_sequence, record.request_sequence, record.instrument_id};
        } else {
            return record.header;
        }
    }, event);
}

bool same_acceptance(const AcceptedOrder& order, const TrackedOrder& tracked) {
    const auto& request = tracked.request;
    return order.order_id == request.order_id && order.participant_id == request.participant_id &&
        order.instrument_id == request.instrument_id && order.side == request.side && order.type == request.type &&
        order.original_quantity.units() == request.quantity_units && order.accepted_at == request.timestamp &&
        order.arrival_sequence.value == tracked.priority &&
        (order.limit_price ? std::optional<std::int64_t>{order.limit_price->ticks()} : std::nullopt) == request.limit_price_ticks;
}

void check_status(const ExchangeEvent& event, const TrackedOrder& tracked) {
    if (tracked.remaining == 0) {
        const auto* filled = std::get_if<OrderFilled>(&event);
        require(filled && filled->order_id == tracked.request.order_id &&
            filled->participant_id == tracked.request.participant_id && filled->remaining_quantity.units() == 0,
            "full-fill status differs from execution reconciliation");
    } else {
        const auto* partial = std::get_if<OrderPartiallyFilled>(&event);
        require(partial && partial->order_id == tracked.request.order_id &&
            partial->participant_id == tracked.request.participant_id && partial->remaining_quantity.units() == tracked.remaining,
            "partial-fill status differs from execution reconciliation");
    }
}

void reconcile(const OrderRequest& request, std::span<const ExchangeEvent> events, Ledger& ledger, std::uint64_t sequence) {
    if (const auto reason = rejection_reason(request, ledger)) {
        const auto* rejected = std::get_if<OrderRejected>(&events.front());
        require(events.size() == 1 && rejected && rejected->reason == *reason && same_request(rejected->request, request),
            "order rejection or preserved request differs");
        ++ledger.categories[5];
        return;
    }
    ++ledger.categories[request.type == OrderType::Market ? 2 : eligible(ledger, request) ? 1 : 0];
    TrackedOrder incoming{request, request.quantity_units, sequence};
    const auto* accepted = std::get_if<OrderAccepted>(&events.front());
    require(accepted && same_acceptance(accepted->order, incoming), "acceptance payload differs from submission");
    require(ledger.accepted.insert(request.order_id).second, "accepted ID reused");
    ledger.accepted_ids.push_back(request.order_id);
    std::size_t cursor = 1;
    while (cursor < events.size() && std::holds_alternative<Execution>(events[cursor])) {
        require(cursor + 2 < events.size() && incoming.remaining > 0 && eligible(ledger, request),
            "execution without remaining quantity, eligible liquidity, or statuses");
        const auto reference = ledger.best(request.side == Side::Buy ? Side::Sell : Side::Buy);
        auto resting = ledger.active.find(reference->first);
        const auto& execution = std::get<Execution>(events[cursor]);
        const bool buy = request.side == Side::Buy;
        require(execution.aggressive_order_id == request.order_id && execution.resting_order_id == resting->first &&
            execution.aggressive_side == request.side && execution.price.ticks() == *resting->second.request.limit_price_ticks &&
            execution.buyer_id == (buy ? request.participant_id : resting->second.request.participant_id) &&
            execution.seller_id == (buy ? resting->second.request.participant_id : request.participant_id),
            "execution violates resting price, ownership, or price/FIFO priority");
        const auto units = std::min(incoming.remaining, resting->second.remaining);
        require(execution.quantity.units() == units, "execution quantity differs from the two remainders");
        // Bounded subtraction avoids overflow even when individual quantities reach INT64_MAX.
        incoming.remaining -= units;
        resting->second.remaining -= units;
        check_status(events[cursor + 1], resting->second);
        check_status(events[cursor + 2], incoming);
        if (resting->second.remaining == 0) ledger.active.erase(resting);
        cursor += 3;
    }
    require(incoming.remaining == 0 || !eligible(ledger, request), "eligible liquidity left unmatched");
    if (incoming.remaining == 0) {
        require(cursor == events.size(), "extra events after complete fill");
    } else {
        require(cursor + 1 == events.size(), "missing or extra final remainder event");
        if (request.type == OrderType::Limit) {
            const auto* rested = std::get_if<OrderRested>(&events[cursor]);
            require(rested && rested->order_id == request.order_id && rested->participant_id == request.participant_id &&
                rested->price.ticks() == *request.limit_price_ticks && rested->remaining_quantity.units() == incoming.remaining,
                "rested remainder differs from executions");
            require(ledger.active.emplace(request.order_id, incoming).second, "incoming ID already active");
        } else {
            const auto* expired = std::get_if<MarketRemainderExpired>(&events[cursor]);
            require(expired && expired->order_id == request.order_id && expired->participant_id == request.participant_id &&
                expired->remaining_quantity.units() == incoming.remaining, "market expiry differs from executions");
        }
    }
}

void reconcile(const CancelRequest& request, std::span<const ExchangeEvent> events, Ledger& ledger, std::uint64_t) {
    require(events.size() == 1, "cancellation must return exactly one event");
    if (const auto reason = rejection_reason(request, ledger)) {
        const auto* rejected = std::get_if<CancelRejected>(&events.front());
        require(rejected && rejected->reason == *reason && same_request(rejected->request, request),
            "cancel rejection or preserved request differs");
        ++ledger.categories[4];
        return;
    }
    ++ledger.categories[3];
    const auto target = ledger.active.find(request.target_order_id);
    const auto* cancelled = std::get_if<OrderCancelled>(&events.front());
    require(cancelled && cancelled->order_id == request.target_order_id && cancelled->participant_id == request.participant_id &&
        cancelled->price.ticks() == *target->second.request.limit_price_ticks &&
        cancelled->cancelled_quantity.units() == target->second.remaining, "cancellation quantity or metadata differs");
    ledger.active.erase(target);
}

void check_book(const Exchange& exchange, const Ledger& ledger, std::uint64_t sequence) {
    const auto& book = exchange.book();
    require(book.check_invariants(), "book structural invariant failed");
    require(book.order_count() == ledger.active.size(), "active count differs from executions/cancellations");
    for (const auto& [id, tracked] : ledger.active) {
        const auto* resting = book.find(id);
        require(resting && same_acceptance(resting->order, tracked) && resting->remaining_quantity.units() == tracked.remaining,
            "active record differs from execution ledger");
    }
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto expected = ledger.best(side);
        const auto* head = side == Side::Buy ? book.best_bid() : book.best_ask();
        require(expected == ledger.active.end() ? head == nullptr : head && head->order.order_id == expected->first,
            "best/FIFO head differs from ledger");
    }
    require(exchange.current_time().nanoseconds == ledger.time && exchange.last_request_sequence().value == sequence &&
        exchange.last_event_sequence().value == ledger.events, "committed clock or counters differ");
}

Command generate(std::mt19937_64& random, const Ledger& ledger) {
    // Use raw engine values with explicit modulo mapping, independent of distribution implementations.
    const auto draw = [&](std::uint64_t bound) { return random() % bound; };
    auto kind = draw(6);
    if (ledger.active.size() >= 64 && kind < 2) kind = 2;
    if (kind == 3 && ledger.active.empty()) kind = 0;
    const auto time = ledger.time + draw(2);
    if (kind == 3 || kind == 4) {
        OrderId unknown{random()};
        while (unknown.value == 0 || ledger.active.contains(unknown)) unknown = {random()};
        CancelRequest cancel{{time}, {1}, {1}, unknown};
        if (!ledger.active.empty() && (kind == 3 || draw(2) == 0)) {
            auto target = ledger.active.begin();
            std::advance(target, static_cast<std::ptrdiff_t>(draw(ledger.active.size())));
            cancel.target_order_id = target->first;
            cancel.participant_id = target->second.request.participant_id;
            if (kind == 4) cancel.participant_id.value = cancel.participant_id.value % 3 + 1;
        }
        return cancel;
    }
    const auto side = draw(2) == 0 ? Side::Buy : Side::Sell;
    const auto units = draw(32) == 0 ? maximum_quantity : static_cast<std::int64_t>(draw(20) + 1);
    OrderId fresh{random()};
    while (fresh.value == 0 || ledger.accepted.contains(fresh)) fresh = {random()};
    OrderRequest order{{time}, {1}, {draw(3) + 1}, fresh, side, OrderType::Limit, units, 100};
    const auto opposite = ledger.best(side == Side::Buy ? Side::Sell : Side::Buy);
    if (kind == 2) {
        order.type = OrderType::Market;
        order.limit_price_ticks.reset();
    } else if (opposite != ledger.active.end()) {
        const auto price = *opposite->second.request.limit_price_ticks;
        const auto distance = static_cast<std::int64_t>(draw(5));
        if (kind == 0) {
            if ((side == Side::Buy && price == 1) || (side == Side::Sell && price == maximum_quantity)) {
                order.type = OrderType::Market;
                order.limit_price_ticks.reset();
            } else {
                order.limit_price_ticks = side == Side::Buy ? price - 1 - std::min(distance, price - 2)
                    : price + 1 + std::min(distance, maximum_quantity - price - 1);
            }
        } else {
            order.limit_price_ticks = side == Side::Buy ? price + std::min(distance, maximum_quantity - price)
                : price - std::min(distance, price - 1);
        }
    } else {
        order.limit_price_ticks = static_cast<std::int64_t>(draw(21) + 90);
    }
    if (kind == 5) {
        switch (draw(8)) {
        case 0: order.quantity_units = 0; break;
        case 1: order.limit_price_ticks = 0; break;
        case 2: order.participant_id = {4}; break;
        case 3: order.instrument_id = {2}; break;
        case 4: order.side = static_cast<Side>(0); break;
        case 5:
            if (ledger.time > 0) order.timestamp = {ledger.time - 1};
            else order.quantity_units = -1;
            break;
        case 6:
            if (!ledger.accepted_ids.empty()) order.order_id = ledger.accepted_ids[draw(ledger.accepted_ids.size())];
            else order.order_id = {};
            break;
        case 7: order.type = static_cast<OrderType>(0); break;
        }
    }
    return order;
}

struct WorkloadResult {
    std::vector<Command> history;
    std::array<std::size_t, 6> categories{};
    std::uint64_t events{};
    std::size_t failed_operation{};
    std::string failure;
};

WorkloadResult run(std::uint64_t seed, std::size_t operations, std::optional<std::size_t> inject_at = {}, bool inject_first = false) {
    const std::array participants{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}};
    Exchange exchange{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    Ledger ledger;
    std::mt19937_64 random{seed};
    WorkloadResult result;
    result.history.reserve(operations);
    try {
        for (std::size_t operation = 1; operation <= operations; ++operation) {
            const auto command = generate(random, ledger);
            result.history.push_back(command);
            auto output = std::visit([&](const auto& request) {
                if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) return exchange.process_order(request);
                else return exchange.process_cancel(request);
            }, command);
            require(output.has_value() && !output->empty(), "engine error or empty output");
            if (inject_first || inject_at == operation) {
                bool injected = false;
                for (auto& event : *output) {
                    if (auto* execution = std::get_if<Execution>(&event)) {
                        const auto units = execution->quantity.units();
                        execution->quantity = Quantity::from_units(units == maximum_quantity ? units - 1 : units + 1).value();
                        injected = true;
                        break;
                    }
                }
                require(inject_at != operation || injected, "selected injection operation has no execution");
            }
            const auto request_time = std::visit([](const auto& request) { return request.timestamp.nanoseconds; }, command);
            const auto instrument = std::visit([](const auto& request) { return request.instrument_id; }, command);
            const auto time = std::max(ledger.time, request_time);
            for (std::size_t index = 0; index < output->size(); ++index) {
                require(marketlab::is_valid((*output)[index]), "invalid event record");
                const auto header = event_header((*output)[index]);
                require(header.timestamp.nanoseconds == time && header.instrument_id == instrument &&
                    header.request_sequence.value == operation && header.event_sequence.value == ledger.events + index + 1,
                    "event clock, instrument, or sequence differs");
            }
            std::visit([&](const auto& request) { reconcile(request, *output, ledger, operation); }, command);
            ledger.time = time;
            ledger.events += output->size();
            check_book(exchange, ledger, operation);
        }
    } catch (const std::exception& error) {
        result.failed_operation = result.history.size();
        result.failure = error.what();
    }
    result.categories = ledger.categories;
    result.events = ledger.events;
    return result;
}

void print_failure(std::uint64_t seed, std::size_t operations, const WorkloadResult& result,
    std::optional<std::size_t> injected = {}) {
    std::cerr << "seed=" << seed << " operation=" << result.failed_operation << ": " << result.failure << '\n';
    std::cerr << "replay: exchange_property_tests " << seed << ' ' << operations;
    if (injected) std::cerr << ' ' << *injected;
    std::cerr << "\nrequest prefix (time instrument participant id side type quantity price; or cancel time instrument participant target):\n";
    for (std::size_t index = 0; index < result.history.size(); ++index) {
        std::cerr << index + 1 << ' ';
        std::visit([](const auto& request) {
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                std::cerr << "order " << request.timestamp.nanoseconds << ' ' << request.instrument_id.value << ' '
                    << request.participant_id.value << ' ' << request.order_id.value << ' ' << static_cast<int>(request.side) << ' '
                    << static_cast<int>(request.type) << ' ' << request.quantity_units << ' ';
                if (request.limit_price_ticks) std::cerr << *request.limit_price_ticks;
                else std::cerr << "none";
            } else {
                std::cerr << "cancel " << request.timestamp.nanoseconds << ' ' << request.instrument_id.value << ' '
                    << request.participant_id.value << ' ' << request.target_order_id.value;
            }
        }, result.history[index]);
        std::cerr << '\n';
    }
}

bool same_history(const WorkloadResult& left, const WorkloadResult& right) {
    return std::equal(left.history.begin(), left.history.end(), right.history.begin(), right.history.end(), same_command);
}

std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

int main(int argc, char** argv) {
    if (argc != 1) {
        const auto seed = argc >= 3 ? number(argv[1]) : std::nullopt;
        const auto operations = argc >= 3 ? number(argv[2]) : std::nullopt;
        const auto injection = argc == 4 ? number(argv[3]) : std::optional<std::uint64_t>{};
        if ((argc != 3 && argc != 4) || !seed || !operations || *operations == 0 || *operations > 1000000 ||
            (argc == 4 && (!injection || *injection == 0 || *injection > *operations))) {
            std::cerr << "usage: exchange_property_tests [seed operations [injected_operation]]; operations: 1..1000000\n";
            return 2;
        }
        const auto result = run(*seed, static_cast<std::size_t>(*operations),
            injection ? std::optional<std::size_t>{static_cast<std::size_t>(*injection)} : std::nullopt);
        if (result.failed_operation) {
            print_failure(*seed, static_cast<std::size_t>(*operations), result,
                injection ? std::optional<std::size_t>{static_cast<std::size_t>(*injection)} : std::nullopt);
            return 1;
        }
        return 0;
    }
    for (const auto seed : std::array<std::uint64_t, 4>{1, 42, 1729, std::numeric_limits<std::uint64_t>::max()}) {
        constexpr std::size_t operations = 5000;
        const auto first = run(seed, operations);
        const auto second = run(seed, operations);
        if (first.failed_operation || second.failed_operation) {
            print_failure(seed, operations, first.failed_operation ? first : second);
            return 1;
        }
        if (!same_history(first, second) || first.events != second.events || first.categories != second.categories ||
            !std::ranges::all_of(first.categories, [](auto count) { return count > 0; })) {
            auto failure = second;
            failure.failed_operation = operations;
            failure.failure = "seed replay or operation-category coverage differs";
            print_failure(seed, operations, failure);
            return 1;
        }
    }
    // Perturb a returned execution only; production state and matching are untouched.
    const auto broken = run(42, 256, {}, true);
    if (!broken.failed_operation || broken.failure != "execution quantity differs from the two remainders") {
        auto failure = broken;
        if (!failure.failed_operation) {
            failure.failed_operation = failure.history.size();
            failure.failure = "deliberate execution error was not detected";
        }
        print_failure(42, 256, failure, failure.failed_operation);
        return 1;
    }
    const auto replay = run(42, broken.failed_operation, broken.failed_operation);
    if (replay.failed_operation != broken.failed_operation || replay.failure != broken.failure || !same_history(broken, replay)) {
        auto failure = replay;
        if (!failure.failed_operation) {
            failure.failed_operation = broken.failed_operation;
            failure.failure = "recorded failure did not reproduce";
        }
        print_failure(42, broken.failed_operation, failure, broken.failed_operation);
        return 1;
    }
}
