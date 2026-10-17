#include "participant_ledger_test_support.hpp"

#include <marketlab/participant_fills.hpp>
#include <marketlab/participant_pnl.hpp>
#include <marketlab/simulation.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <optional>
#include <vector>

namespace {
std::ptrdiff_t allocations_before_failure = -1;
}

void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) throw std::bad_alloc{};
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

using namespace ledger_test;

void test_failure_and_retry() {
    for (const std::size_t existing : {std::size_t{0}, std::size_t{1}, std::size_t{8}}) {
        std::size_t failures = 0;
        for (std::ptrdiff_t fault = 0; ; ++fault) {
            ParticipantLedger ledger{{1}, participants};
            std::vector<Execution> prior;
            for (std::size_t index = 0; index < existing; ++index) prior.push_back(trade(index + 1, index + 1, index, 2));
            require(ledger.apply(prior).has_value(), "allocation preparation failed");
            const auto before = ledger.account({1});
            const auto seller_before = ledger.account({2});
            const auto* borrowed = ledger.history().data();
            const std::array batch{
                trade(existing + 10, existing + 10, existing + 10, 5, 2, 1),
                trade(existing + 11, existing + 11, existing + 11, 7, 3, 3),
                trade(existing + 12, existing + 12, existing + 12, 3, 1, 3)};
            bool failed = false;
            allocations_before_failure = fault;
            try {
                const auto result = ledger.apply(batch);
                allocations_before_failure = -1;
                require(result.has_value(), "valid allocation batch refused");
            } catch (const std::bad_alloc&) {
                allocations_before_failure = -1;
                failed = true;
                ++failures;
            }
            if (failed) {
                require(ledger.account({1}) == before && ledger.account({2}) == seller_before &&
                    ledger.account({3}) == ParticipantAccount{} && ledger.history().data() == borrowed &&
                    std::equal(ledger.history().begin(), ledger.history().end(), prior.begin(), prior.end(), same_execution) &&
                    ledger.last_event_sequence().value == existing, "allocation failure changed ledger or borrowed history");
                require(ledger.apply(batch).has_value(), "accounting retry failed");
            }
            require(ledger.history().size() == existing + batch.size() &&
                ledger.account({1}) == account_at_100(2 * existing + 3, 5) &&
                ledger.account({2}) == account_at_100(5, 2 * existing) &&
                ledger.account({3}) == account_at_100(7, 10) &&
                std::equal(ledger.history().begin(), ledger.history().begin() + static_cast<std::ptrdiff_t>(existing),
                    prior.begin(), prior.end(), same_execution) &&
                std::equal(ledger.history().begin() + static_cast<std::ptrdiff_t>(existing), ledger.history().end(),
                    batch.begin(), batch.end(), same_execution), "successful retry lost or doubled accounting");
            if (!failed) {
                // Fixed participant preparation allocates three map nodes, then history grows.
                require(failures >= participants.size() + 1, "did not cover account preparation and history allocation");
                break;
            }
        }
    }
}

void test_reads_and_refusals_do_not_allocate() {
    ParticipantLedger ledger{{1}, participants};
    const std::array first{trade(2, 1, 0, 5)};
    require(ledger.apply(first).has_value(), "read test preparation failed");
    const auto* borrowed = ledger.history().data();
    ParticipantLedger empty_ledger{{1}, participants};
    auto invalid = trade(4, 2, 1, 1);
    invalid.seller_id = {99};
    const std::array invalid_batch{trade(3, 2, 1, 1), invalid};
    allocations_before_failure = 0;
    const auto instrument = ledger.instrument_id();
    const auto account = ledger.account({1});
    const auto missing = ledger.account({99});
    const auto position = account->position();
    const auto cash = account->cash_flow();
    const auto notional = CashAmount::from_aggregate(Price::from_ticks(100).value(), position.magnitude);
    const auto pnl = account->total_pnl(Price::from_ticks(101).value());
    const auto unknown_fifo = fifo_pnl(ledger, {99}, Price::from_ticks(100).value());
    const auto empty_fifo = fifo_pnl(empty_ledger, {1}, Price::from_ticks(100).value());
    const auto fills = participant_fills(ledger, {1});
    const auto missing_fills = participant_fills(ledger, {99});
    const auto empty_fills = participant_fills(empty_ledger, {1});
    const auto history = ledger.history();
    const auto last = ledger.last_event_sequence();
    const auto empty = ledger.apply({});
    const auto duplicate = ledger.apply(first);
    const auto refused = ledger.apply(invalid_batch);
    allocations_before_failure = -1;
    require(instrument.value == 1 && account == account_at_100(5, 0) && !missing &&
        position == NetPosition{false, {0, 5}} && cash == NetCashFlow{true, {0, 0, 500}} &&
        notional == CashAmount{0, 0, 500} && pnl == TotalPnl{false, {0, 0, 5}} &&
        !unknown_fifo && empty_fifo == PnlBreakdown{} &&
        fills == ParticipantFills{1, 0, 1, 0} && !missing_fills && empty_fills == ParticipantFills{} &&
        history.size() == 1 && history.data() == borrowed &&
        last.value == 2 && empty && !duplicate && duplicate.error() == AccountingError::OutOfOrder &&
        !refused && refused.error() == AccountingError::UnknownParticipant && ledger.history().size() == 1,
        "read, empty batch or diagnostic allocated or changed state");
}

void test_fifo_query_failure_and_retry() {
    ParticipantLedger ledger{{1}, participants};
    std::vector<Execution> executions;
    for (std::uint64_t index = 0; index < 400; ++index) {
        auto execution = trade(index + 1, index + 1, index, 1);
        execution.price = Price::from_ticks(static_cast<std::int64_t>(100 + index)).value();
        executions.push_back(execution);
    }
    auto closing = trade(401, 401, 400, 401, 2, 1);
    closing.price = Price::from_ticks(1000).value();
    executions.push_back(closing);
    require(ledger.apply(executions).has_value(), "FIFO allocation preparation failed");
    const auto buyer = ledger.account({1});
    const auto seller = ledger.account({2});
    const auto* borrowed = ledger.history().data();
    const auto mark = Price::from_ticks(900).value();
    const PnlBreakdown expected{{false, {0, 0, 280200}}, {false, {0, 0, 100}}, {false, {0, 0, 280300}}};
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        bool failed = false;
        allocations_before_failure = fault;
        try {
            const auto result = fifo_pnl(ledger, {1}, mark);
            allocations_before_failure = -1;
            require(result == expected, "FIFO allocation result differs");
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            failed = true;
            ++failures;
        }
        require(ledger.account({1}) == buyer && ledger.account({2}) == seller &&
            ledger.history().data() == borrowed && ledger.last_event_sequence().value == 401 &&
            std::equal(ledger.history().begin(), ledger.history().end(), executions.begin(), executions.end(), same_execution),
            "FIFO query failure changed accounts or borrowed history");
        require(fifo_pnl(ledger, {1}, mark) == expected, "FIFO query retry differs");
        if (!failed) break;
    }
    require(failures != 0, "FIFO temporary allocation failures were not exercised");
}

void test_simulation_output_retry() {
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        Simulation simulation{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
        ParticipantLedger ledger{{1}, participants};
        require(simulation.schedule(OrderRequest{{0}, {1}, {2}, {1}, Side::Sell, OrderType::Limit, 3, 101}).has_value() &&
            simulation.schedule(OrderRequest{{0}, {1}, {3}, {2}, Side::Sell, OrderType::Limit, 5, 102}).has_value() &&
            simulation.schedule(OrderRequest{{10}, {1}, {1}, {3}, Side::Buy, OrderType::Limit, 10, 103}).has_value() &&
            simulation.schedule(CancelRequest{{20}, {1}, {1}, {99}}).has_value(), "retry simulation scheduling failed");
        for (int index = 0; index < 2; ++index) {
            const auto step = simulation.step();
            require(step && *step, "retry simulation preparation failed");
        }
        const auto delivered = simulation.step();
        require(delivered && *delivered && delivered->value().events.size() == 8, "retry multilevel delivery failed");
        std::vector<Execution> executions;
        for (const auto& event : delivered->value().events) {
            if (const auto* execution = std::get_if<Execution>(&event)) executions.push_back(*execution);
        }
        const auto* pending = simulation.queue().next();
        bool failed = false;
        allocations_before_failure = fault;
        try {
            const auto result = ledger.apply(executions);
            allocations_before_failure = -1;
            require(result.has_value(), "committed execution batch refused");
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            failed = true;
        }
        if (failed) {
            require(ledger.history().empty() && ledger.account({1}) == ParticipantAccount{} &&
                ledger.account({2}) == ParticipantAccount{} && ledger.account({3}) == ParticipantAccount{},
                "failed accounting changed committed balances");
            require(ledger.apply(executions).has_value(), "retained exchange output could not be retried");
        }
        const auto& exchange = simulation.exchange();
        const auto* remainder = exchange.book().find({3});
        require(simulation.current_time().nanoseconds == 10 && simulation.queue().size() == 1 &&
            simulation.queue().next() == pending && pending && pending->sequence.value == 4 &&
            exchange.last_request_sequence().value == 3 && exchange.last_event_sequence().value == 12 &&
            exchange.cumulative_volume() == AggregateQuantity{0, 8} && exchange.book().order_count() == 1 &&
            remainder && remainder->remaining_quantity.units() == 2 && exchange.book().check_invariants(),
            "accounting retry reprocessed an exchange request or changed pending delivery");
        require(ledger.account({1}) == ParticipantAccount{{0, 8}, {}, {0, 0, 813}, {}} &&
            ledger.account({2}) == ParticipantAccount{{}, {0, 3}, {}, {0, 0, 303}} &&
            ledger.account({3}) == ParticipantAccount{{}, {0, 5}, {}, {0, 0, 510}} &&
            std::equal(ledger.history().begin(), ledger.history().end(), executions.begin(), executions.end(), same_execution),
            "accounting retry lost or doubled committed fills");
        require(fifo_pnl(ledger, {1}, Price::from_ticks(103).value()) ==
            PnlBreakdown{{}, {false, {0, 0, 11}}, {false, {0, 0, 11}}},
            "retained simulation FIFO projection differs");
        if (!failed) break;
    }
}

int main() {
    try {
        test_failure_and_retry();
        test_fifo_query_failure_and_retry();
        test_simulation_output_retry();
        test_reads_and_refusals_do_not_allocate();
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
