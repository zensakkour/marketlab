#include <marketlab/order_book.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {

// Fail one allocation at a chosen point inside a book operation, never during setup.
std::ptrdiff_t allocations_before_failure = -1;

} // namespace

void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) {
        throw std::bad_alloc{};
    }
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) {
        return allocation;
    }
    throw std::bad_alloc{};
}

void operator delete(void* allocation) noexcept {
    std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
    ::operator delete(allocation);
}

void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}

void operator delete[](void* allocation) noexcept {
    ::operator delete(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    ::operator delete(allocation);
}

bool test_limit_allocations() {
    using marketlab::AcceptedOrder;
    using marketlab::InstrumentId;
    using marketlab::OrderBook;
    using marketlab::OrderType;
    using marketlab::PassiveInsertResult;
    using marketlab::Price;
    using marketlab::Quantity;
    using marketlab::Side;
    const auto make_order = [](std::uint64_t id, Side side, std::int64_t price, int units) {
        return AcceptedOrder{.order_id = {id}, .participant_id = {1}, .instrument_id = {1},
            .side = side, .type = OrderType::Limit, .original_quantity = Quantity::from_units(units).value(),
            .limit_price = Price::from_ticks(price).value(), .accepted_at = {0}, .arrival_sequence = {id}};
    };
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        const int direction = side == Side::Buy ? 1 : -1;
        for (const int existing : {0, 5, 6, 7}) {
            for (const bool crossing : {false, true}) {
                for (const int units : {2, 8, 12}) {
                    for (std::ptrdiff_t fault = 0; ; ++fault) {
                        OrderBook book{InstrumentId{1}};
                        for (int index = 0; index < existing; ++index) {
                            const auto own = make_order(static_cast<std::uint64_t>(index + 1), side, 100, 5);
                            if (book.insert_passive({own, own.original_quantity}) != PassiveInsertResult::Inserted) {
                                return false;
                            }
                        }
                        const auto first = make_order(10, opposite, 100 + direction, 3);
                        const auto second = make_order(11, opposite, 100 + 2 * direction, 5);
                        if (book.insert_passive({first, first.original_quantity}) != PassiveInsertResult::Inserted ||
                            book.insert_passive({second, second.original_quantity}) != PassiveInsertResult::Inserted) {
                            return false;
                        }
                        const auto incoming = make_order(20, side, crossing ? 100 + 2 * direction : 100, units);
                        const auto* original_bid = book.best_bid();
                        const auto* original_ask = book.best_ask();
                        bool failed = false;
                        allocations_before_failure = fault;
                        try {
                            const auto result = book.process_limit(incoming, {100});
                            allocations_before_failure = -1;
                            const auto remaining = crossing ? (units > 8 ? units - 8 : 0) : units;
                            const std::size_t executions = crossing ? (units <= 3 ? 1 : 2) : 0;
                            if (!result || result->remaining_quantity.units() != remaining ||
                                result->events.size() != 1 + 3 * executions + (remaining > 0 ? 1 : 0) ||
                                book.contains(incoming.order_id) != (remaining > 0) ||
                                book.order_count() != static_cast<std::size_t>(existing) +
                                    (crossing ? (units < 3 ? 2 : units < 8 ? 1 : 0) : 2) + (remaining > 0 ? 1 : 0)) {
                                return false;
                            }
                        } catch (const std::bad_alloc&) {
                            allocations_before_failure = -1;
                            failed = true;
                        }
                        if (!failed) {
                            break;
                        }
                        if (book.order_count() != static_cast<std::size_t>(existing + 2) ||
                            book.best_bid() != original_bid || book.best_ask() != original_ask ||
                            (side == Side::Buy ? book.best_ask() : book.best_bid())->remaining_quantity.units() != 3 ||
                            (existing && (side == Side::Buy ? book.best_bid() : book.best_ask())->remaining_quantity.units() != 5) ||
                            book.contains(incoming.order_id) || !book.contains(first.order_id) || !book.contains(second.order_id)) {
                            std::cerr << "limit allocation failure changed active state\n";
                            return false;
                        }
                        // Retrying the identical accepted order checks ID/priority rollback
                        // and exposes any partial liquidity consumption beyond the first head.
                        const auto retry = book.process_limit(incoming, {100});
                        const auto remaining = crossing ? (units > 8 ? units - 8 : 0) : units;
                        if (!retry || retry->remaining_quantity.units() != remaining) {
                            std::cerr << "limit allocation failure prevented an identical retry\n";
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

int main() {
    if (!test_limit_allocations()) {
        return 1;
    }
    using marketlab::AcceptedOrder;
    using marketlab::InstrumentId;
    using marketlab::OrderBook;
    using marketlab::OrderType;
    using marketlab::PassiveInsertResult;
    using marketlab::Price;
    using marketlab::Quantity;
    using marketlab::RestingOrder;
    using marketlab::Side;
    constexpr auto quantity = Quantity::from_units(5).value();
    constexpr RestingOrder resting{
        AcceptedOrder{
            .order_id = {1},
            .participant_id = {2},
            .instrument_id = {1},
            .side = Side::Buy,
            .type = OrderType::Limit,
            .original_quantity = quantity,
            .limit_price = Price::from_ticks(100).value(),
            .accepted_at = {0},
            .arrival_sequence = {1},
        },
        quantity,
    };

    // Exercise every allocation point until insertion succeeds without reaching
    // the injected failure. Repeating with existing orders also checks preservation.
    for (const int existing_orders : {0, 5, 6, 7}) {
        for (std::ptrdiff_t failure_point = 0; ; ++failure_point) {
            OrderBook book{InstrumentId{1}};
            for (int index = 0; index < existing_orders; ++index) {
                auto existing = resting;
                existing.order.order_id = {static_cast<std::uint64_t>(index + 1)};
                existing.order.arrival_sequence = {static_cast<std::uint64_t>(index + 1)};
                if (book.insert_passive(existing) != PassiveInsertResult::Inserted) {
                    std::cerr << "could not prepare allocation test\n";
                    return 1;
                }
            }
            auto incoming = resting;
            incoming.order.order_id = {static_cast<std::uint64_t>(existing_orders + 1)};
            incoming.order.arrival_sequence = {static_cast<std::uint64_t>(existing_orders + 1)};
            const auto* original_bid = book.best_bid();
            bool failed = false;
            PassiveInsertResult result{};
            allocations_before_failure = failure_point;
            try {
                result = book.insert_passive(incoming);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            allocations_before_failure = -1;

            if (!failed) {
                if (result != PassiveInsertResult::Inserted ||
                    book.order_count() != static_cast<std::size_t>(existing_orders + 1)) {
                    std::cerr << "insertion failed after allocation faults were exhausted\n";
                    return 1;
                }
                break;
            }
            if (book.order_count() != static_cast<std::size_t>(existing_orders) ||
                book.contains(incoming.order.order_id) || book.best_bid() != original_bid ||
                book.best_ask() || (original_bid && original_bid->remaining_quantity != quantity)) {
                std::cerr << "allocation failure changed the book\n";
                return 1;
            }
            if (book.insert_passive(incoming) != PassiveInsertResult::Inserted ||
                !book.contains(incoming.order.order_id)) {
                std::cerr << "allocation failure left an ID, price level, or sequence behind\n";
                return 1;
            }
        }
    }

    OrderBook matching{InstrumentId{1}};
    if (matching.insert_passive(resting) != PassiveInsertResult::Inserted) {
        return 1;
    }
    for (const int units : {2, 3}) {
        auto incoming = resting.order;
        incoming.order_id = {static_cast<std::uint64_t>(units)};
        incoming.arrival_sequence = {static_cast<std::uint64_t>(units)};
        incoming.side = Side::Sell;
        incoming.original_quantity = Quantity::from_units(units).value();
        allocations_before_failure = 0;
        try {
            const auto result = matching.match_one(incoming, incoming.original_quantity, {1});
            allocations_before_failure = -1;
            if (!result || !*result || (**result).execution.quantity.units() != units ||
                (**result).incoming_remaining.units() != 0 ||
                (**result).resting_remaining.units() != (units == 2 ? 3 : 0)) {
                std::cerr << "allocation-free matching produced an incorrect fill\n";
                return 1;
            }
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            std::cerr << "matching attempted to allocate\n";
            return 1;
        }
    }
    if (!matching.empty() || matching.contains(resting.order.order_id) || matching.best_bid()) {
        std::cerr << "allocation-free full fill left active state\n";
        return 1;
    }
}