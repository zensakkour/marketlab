#include "participant_ledger_test_support.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace ledger_test;

constexpr NetPosition zero_position;
static_assert(zero_position == NetPosition{false, {}});
constexpr ParticipantAccount flat{};
static_assert(flat.position() == NetPosition{false, {}});
constexpr ParticipantAccount long_one{{1, 0}, {0, std::numeric_limits<std::uint64_t>::max()}};
static_assert(long_one.position() == NetPosition{false, {0, 1}});
constexpr ParticipantAccount short_one{long_one.sell_volume, long_one.buy_volume};
static_assert(short_one.position() == NetPosition{true, {0, 1}});
static_assert(noexcept(flat.position()));
static_assert(noexcept(flat.cash_flow()));
static_assert(flat.cash_flow() == NetCashFlow{});
static_assert(!std::is_convertible_v<NetPosition, Quantity>);
static_assert(std::is_same_v<decltype(std::declval<const ParticipantLedger&>().history()), std::span<const Execution>>);
static_assert(!std::is_copy_constructible_v<ParticipantLedger>);

void check_same(const ParticipantLedger& left, const ParticipantLedger& right) {
    require(left.instrument_id() == right.instrument_id() && left.last_event_sequence() == right.last_event_sequence() &&
        std::equal(left.history().begin(), left.history().end(), right.history().begin(), right.history().end(), same_execution),
        "ledger replay history differs");
    for (const auto id : participants) require(left.account(id) == right.account(id), "ledger replay balances differ");
}

void test_configuration() {
    bool rejected = false;
    try { ParticipantLedger invalid{{0}, participants}; }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "zero ledger instrument accepted");
    rejected = false;
    const std::array bad{ParticipantId{1}, ParticipantId{0}};
    try { ParticipantLedger invalid{{1}, bad}; }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "zero participant registration accepted");
    const std::array repeated{ParticipantId{1}, ParticipantId{1}};
    ParticipantLedger ledger{{1}, repeated};
    require(ledger.account({1}) == ParticipantAccount{} && !ledger.account({0}) && !ledger.account({2}) &&
        ledger.history().empty() && ledger.last_event_sequence().value == 0, "initial ledger state differs");
    require(ledger.apply({}).has_value(), "empty batch failed");
    ParticipantLedger empty{{1}, {}};
    const std::array executions{trade(1, 1, 0, 5)};
    const auto result = empty.apply(executions);
    require(!result && result.error() == AccountingError::UnknownParticipant && empty.history().empty(),
        "empty participant set accepted execution");
}

void test_balances_and_replay() {
    ParticipantLedger ledger{{1}, participants};
    const std::array first{trade(2, 1, 0, 5)};
    require(ledger.apply(first).has_value(), "first execution failed");
    const auto saved = ledger.account({1});
    const std::array next{trade(8, 3, 1, 2, 2, 1), trade(11, 4, 2, 4, 1, 1)};
    require(ledger.apply(next).has_value(), "round-trip/self-match batch failed");
    require(ledger.account({1}) == account_at_100(9, 6) &&
        ledger.account({2}) == account_at_100(2, 5) &&
        ledger.account({3}) == ParticipantAccount{}, "gross volumes or unused participant differ");
    require(ledger.account({1})->position() == NetPosition{false, {0, 3}} &&
        ledger.account({2})->position() == NetPosition{true, {0, 3}}, "net positions do not reconcile");
    require(saved == account_at_100(5, 0) && ledger.history().size() == 3 &&
        same_execution(ledger.history()[0], first[0]) && same_execution(ledger.history()[1], next[0]) &&
        same_execution(ledger.history()[2], next[1]), "owned account or exact execution history differs");
    const auto* borrowed = ledger.history().data();
    require(ledger.apply({}).has_value() && ledger.history().data() == borrowed &&
        ledger.last_event_sequence().value == 11, "empty batch changed history");

    ParticipantLedger replay{{1}, participants};
    require(replay.apply(ledger.history()).has_value(), "whole-history replay failed");
    check_same(ledger, replay);
    ParticipantLedger incremental{{1}, participants};
    for (const auto& execution : ledger.history()) {
        require(incremental.apply(std::span{&execution, 1}).has_value(), "incremental replay failed");
    }
    check_same(ledger, incremental);
    auto detached = ledger.account({1});
    detached->buy_volume = {std::numeric_limits<std::uint64_t>::max(), 0};
    require(ledger.account({1}) == account_at_100(9, 6), "caller mutated ledger through account copy");
    const auto duplicate = ledger.apply(ledger.history());
    require(!duplicate && duplicate.error() == AccountingError::OutOfOrder &&
        ledger.history().data() == borrowed, "duplicate history was applied or invalidated a borrowed view");
    check_same(ledger, replay);
}

void test_failed_batches() {
    struct Case { Execution record; AccountingError error; };
    const auto valid = trade(30, 12, 12, 7);
    std::vector<Case> cases;
    const auto add = [&](auto change, AccountingError error) {
        auto execution = valid;
        change(execution);
        cases.push_back({execution, error});
    };
    add([](auto& e) { e.event_sequence = {}; }, AccountingError::InvalidExecution);
    add([](auto& e) { e.request_sequence = {}; }, AccountingError::InvalidExecution);
    add([](auto& e) { e.instrument_id = {}; }, AccountingError::InvalidExecution);
    add([](auto& e) { e.buyer_id = {}; }, AccountingError::InvalidExecution);
    add([](auto& e) { e.seller_id = {}; }, AccountingError::InvalidExecution);
    add([](auto& e) { e.aggressive_order_id = {}; }, AccountingError::InvalidExecution);
    add([](auto& e) { e.resting_order_id = e.aggressive_order_id; }, AccountingError::InvalidExecution);
    add([](auto& e) { e.aggressive_side = static_cast<Side>(0); }, AccountingError::InvalidExecution);
    add([](auto& e) { e.quantity = Quantity::from_units(0).value(); }, AccountingError::InvalidExecution);
    add([](auto& e) { e.instrument_id = {2}; }, AccountingError::WrongInstrument);
    add([](auto& e) { e.buyer_id = {4}; }, AccountingError::UnknownParticipant);
    add([](auto& e) { e.seller_id = {4}; }, AccountingError::UnknownParticipant);
    add([](auto& e) { e.event_sequence = {20}; }, AccountingError::OutOfOrder);
    add([](auto& e) { e.event_sequence = {19}; }, AccountingError::OutOfOrder);
    add([](auto& e) { e.request_sequence = {10}; }, AccountingError::OutOfOrder);
    add([](auto& e) { e.timestamp = {10}; }, AccountingError::OutOfOrder);
    add([](auto& e) { e.request_sequence = {11}; }, AccountingError::OutOfOrder);
    for (const auto& test : cases) {
        ParticipantLedger ledger{{1}, participants};
        const std::array prior{trade(10, 10, 10, 5)};
        require(ledger.apply(prior).has_value(), "failed-batch preparation failed");
        const auto before = ledger.account({1});
        const auto* borrowed = ledger.history().data();
        const std::array batch{trade(20, 11, 11, 3), test.record};
        const auto result = ledger.apply(batch);
        require(!result && result.error() == test.error && ledger.account({1}) == before &&
            ledger.account({2}) == account_at_100(0, 5) && ledger.account({3}) == ParticipantAccount{} &&
            ledger.history().size() == 1 && ledger.history().data() == borrowed &&
            same_execution(ledger.history().front(), prior.front()) && ledger.last_event_sequence().value == 10,
            "invalid suffix committed a prefix, changed history or reported the wrong error");
        const std::array corrected{batch.front(), valid};
        require(ledger.apply(corrected).has_value() && ledger.account({1})->buy_volume == AggregateQuantity{0, 15},
            "corrected batch could not be retried");
    }
}

void test_large_volumes_and_final_sequence() {
    ParticipantLedger ledger{{1}, participants};
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::array executions{trade(2, 1, 0, maximum), trade(5, 2, 1, maximum), trade(8, 3, 2, maximum)};
    require(ledger.apply(executions).has_value(), "large-volume batch failed");
    const AggregateQuantity expected{1, static_cast<std::uint64_t>(maximum) - 2};
    const auto saved = ledger.account({1}).value();
    require(saved.buy_volume == expected && saved.position() == NetPosition{false, expected} &&
        ledger.account({2})->position() == NetPosition{true, expected} &&
        marketlab::to_string(expected) == "27670116110564327421", "large position narrowed or wrapped");
    auto last = trade(std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(), maximum, 1, 1);
    last.price = Price::from_ticks(maximum).value();
    const std::array final{last};
    require(ledger.apply(final).has_value() && ledger.account({1})->position() == saved.position() &&
        ledger.account({1})->buy_volume == expected.add(last.quantity).value() &&
        ledger.account({1})->sell_volume == AggregateQuantity{0, static_cast<std::uint64_t>(maximum)},
        "maximum-sequence self-match changed net position or lost gross volume");
    const auto repeated = ledger.apply(final);
    require(!repeated && repeated.error() == AccountingError::OutOfOrder &&
        ledger.last_event_sequence().value == std::numeric_limits<std::uint64_t>::max(),
        "final event sequence wrapped or was applied twice");
    ParticipantLedger replay{{1}, participants};
    require(replay.apply(ledger.history()).has_value(), "large-history replay failed");
    check_same(ledger, replay);
}

void test_cash_flows() {
    ParticipantLedger ledger{{1}, participants};
    const std::array trades{trade(2, 1, 0, 5), trade(8, 2, 1, 2, 2, 1), trade(11, 3, 2, 4, 1, 1)};
    require(ledger.apply(trades).has_value(), "cash scenario failed");
    const auto buyer = ledger.account({1}).value();
    require(buyer.cash_flow() == NetCashFlow{true, {0, 0, 300}} &&
        ledger.account({2})->cash_flow() == NetCashFlow{false, {0, 0, 300}} &&
        ledger.account({3})->cash_flow() == NetCashFlow{}, "net cash does not reconcile after reversal/self-match");
    ParticipantLedger self{{1}, participants};
    const std::array self_trade{trade(1, 1, 0, 7, 1, 1)};
    require(self.apply(self_trade).has_value() && self.account({1}) == account_at_100(7, 7) &&
        self.account({1})->cash_flow() == NetCashFlow{}, "self-match created net cash");
    const auto saved = buyer.cash_flow();
    const auto duplicate = ledger.apply(trades);
    require(!duplicate && ledger.account({1})->cash_flow() == saved, "duplicate changed cash");

    ParticipantLedger large{{1}, participants};
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    std::vector<Execution> executions;
    for (std::uint64_t index = 1; index <= 5; ++index) {
        auto execution = trade(2 * index, index, index, maximum);
        execution.price = Price::from_ticks(maximum).value();
        executions.push_back(execution);
    }
    require(large.apply(executions).has_value(), "large cash batch failed");
    const CashAmount expected{1, 0x3ffffffffffffffb, 5};
    require(large.account({1})->buy_notional == expected && large.account({2})->sell_notional == expected &&
        large.account({1})->cash_flow() == NetCashFlow{true, expected} &&
        large.account({2})->cash_flow() == NetCashFlow{false, expected},
        "cash flow narrowed or wrapped beyond 128 bits");
    auto final = trade(std::numeric_limits<std::uint64_t>::max(), 6, 6, maximum, 1, 1);
    final.price = Price::from_ticks(maximum).value();
    require(large.apply(std::span{&final, 1}).has_value() && large.account({1})->cash_flow() == NetCashFlow{true, expected},
        "large self-match changed net cash");
    ParticipantLedger replay{{1}, participants};
    require(replay.apply(large.history()).has_value(), "cash history replay failed");
    check_same(large, replay);
}

int main() {
    try {
        test_cash_flows();
        test_configuration();
        test_balances_and_replay();
        test_failed_batches();
        test_large_volumes_and_final_sequence();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
