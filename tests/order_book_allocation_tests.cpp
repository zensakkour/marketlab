#include <marketlab/order_book.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {

// Fail one allocation at a chosen point inside insertion, never during setup.
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

int main() {
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
