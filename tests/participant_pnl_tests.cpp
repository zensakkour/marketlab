#include <marketlab/participant_pnl.hpp>

#include "participant_ledger_test_support.hpp"
#include "quantity_oracle.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using namespace ledger_test;

constexpr auto mark_100 = Price::from_ticks(100).value();
static_assert(PnlAmount::from_difference({}, {}) == PnlAmount{});
static_assert(PnlAmount::from_difference({0, 0, 3}, {0, 0, 5}) == PnlAmount{true, {0, 0, 2}});
static_assert(noexcept(PnlAmount::from_difference({}, {})));
static_assert(PnlAmount::from_difference({1, 0, 0}, {0, UINT64_MAX, UINT64_MAX}) ==
    PnlAmount{false, {0, 0, 1}});
static_assert(std::is_same_v<TotalPnl, PnlAmount>);

Execution priced_trade(std::uint64_t sequence, std::int64_t units, std::int64_t ticks,
    std::uint64_t buyer = 1, std::uint64_t seller = 2) {
    auto execution = trade(sequence, sequence, sequence, units, buyer, seller);
    execution.price = Price::from_ticks(ticks).value();
    return execution;
}

// Small fixture expectations; full-width cases use exact decimal strings below.
PnlAmount small_pnl(std::int64_t units) {
    return {units < 0, {0, 0, static_cast<std::uint64_t>(units < 0 ? -units : units)}};
}

void check_identity(const PnlBreakdown& result) {
    const auto positive = add_decimal(
        result.realised.negative ? "0" : marketlab::to_string(result.realised.magnitude),
        result.unrealised.negative ? "0" : marketlab::to_string(result.unrealised.magnitude));
    const auto negative = add_decimal(
        result.realised.negative ? marketlab::to_string(result.realised.magnitude) : "0",
        result.unrealised.negative ? marketlab::to_string(result.unrealised.magnitude) : "0");
    const auto expected = decimal_difference(positive, negative);
    require(result.total.negative == expected.first && marketlab::to_string(result.total.magnitude) == expected.second,
        "realised plus unrealised differs from total PnL");
}

void check(const ParticipantLedger& ledger, std::int64_t ticks, std::int64_t realised,
    std::int64_t unrealised, std::int64_t total, ParticipantId participant = {1}) {
    const auto mark = Price::from_ticks(ticks).value();
    const auto result = marketlab::fifo_pnl(ledger, participant, mark);
    require(result == PnlBreakdown{small_pnl(realised), small_pnl(unrealised), small_pnl(total)} &&
        result->total == ledger.account(participant)->total_pnl(mark), "FIFO decomposition differs");
    check_identity(*result);
}

void test_fifo_and_reversals() {
    ParticipantLedger ledger{{1}, participants};
    require(marketlab::fifo_pnl(ledger, {1}, mark_100) == PnlBreakdown{} &&
        !marketlab::fifo_pnl(ledger, {0}, mark_100) && !marketlab::fifo_pnl(ledger, {99}, mark_100),
        "empty or unknown participant projection differs");
    const std::array opening{priced_trade(1, 3, 100), priced_trade(2, 2, 110)};
    require(ledger.apply(opening).has_value(), "FIFO opening failed");
    check(ledger, 115, 0, 55, 55);
    const auto saved = marketlab::fifo_pnl(ledger, {1}, mark_100).value();
    const auto close = priced_trade(3, 4, 120, 2, 1);
    require(ledger.apply(std::span{&close, 1}).has_value(), "FIFO partial close failed");
    check(ledger, 115, 70, 5, 75);
    check(ledger, 105, 70, -5, 65);
    check(ledger, 115, -70, -5, -75, {2});
    require(saved == PnlBreakdown{small_pnl(0), small_pnl(-20), small_pnl(-20)}, "saved projection changed");
    const auto self = priced_trade(4, 9, 500, 1, 1);
    const auto before = ledger.account({1});
    require(ledger.apply(std::span{&self, 1}).has_value(), "FIFO self-match failed");
    check(ledger, 115, 70, 5, 75);
    require(ledger.account({1})->position() == before->position(), "self-match changed FIFO inventory");
    const auto reversal = priced_trade(5, 3, 90, 2, 1);
    require(ledger.apply(std::span{&reversal, 1}).has_value(), "FIFO reversal failed");
    check(ledger, 80, 50, 20, 70);
    const auto cover = priced_trade(6, 2, 95);
    require(ledger.apply(std::span{&cover, 1}).has_value(), "FIFO short cover failed");
    check(ledger, 1, 40, 0, 40);
    check(ledger, std::numeric_limits<std::int64_t>::max(), 40, 0, 40);
    const auto* borrowed = ledger.history().data();
    check(ledger, 100, 40, 0, 40);
    require(ledger.history().data() == borrowed && ledger.last_event_sequence().value == 6,
        "FIFO query changed history");
    ParticipantLedger rebuilt{{1}, participants};
    require(rebuilt.apply(ledger.history()).has_value() &&
        marketlab::fifo_pnl(rebuilt, {1}, mark_100) == marketlab::fifo_pnl(ledger, {1}, mark_100),
        "FIFO reconstruction differs");
}

void test_short_fifo_and_self_matches() {
    ParticipantLedger ledger{{1}, participants};
    const std::array executions{
        priced_trade(1, 7, 700, 1, 1), priced_trade(2, 3, 110, 2, 1),
        priced_trade(3, 2, 100, 2, 1), priced_trade(4, 4, 90)};
    require(ledger.apply(executions).has_value(), "short FIFO preparation failed");
    check(ledger, 95, 70, 5, 75);
    const auto self = priced_trade(5, 8, 1000, 1, 1);
    require(ledger.apply(std::span{&self, 1}).has_value(), "short FIFO self-match failed");
    check(ledger, 105, 70, -5, 65);
    const auto reversal = priced_trade(6, 2, 120);
    require(ledger.apply(std::span{&reversal, 1}).has_value(), "short-to-long FIFO reversal failed");
    check(ledger, 100, 50, -20, 30);
}

void test_three_participant_reconciliation() {
    ParticipantLedger ledger{{1}, participants};
    const std::array executions{priced_trade(1, 1, 100), priced_trade(2, 1, 110, 3, 1)};
    require(ledger.apply(executions).has_value(), "three-party preparation failed");
    check(ledger, 110, 10, 0, 10);
    check(ledger, 110, 0, -10, -10, {2});
    check(ledger, 110, 0, 0, 0, {3});
}

void test_full_width() {
    ParticipantLedger ledger{{1}, participants};
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::array opening{
        priced_trade(1, maximum, maximum), priced_trade(2, maximum, maximum), priced_trade(3, maximum, maximum)};
    require(ledger.apply(opening).has_value(), "full-width FIFO opening failed");
    const auto loss = marketlab::fifo_pnl(ledger, {1}, Price::from_ticks(1).value()).value();
    require(loss.realised == PnlAmount{} && loss.unrealised.negative &&
        marketlab::to_string(loss.unrealised.magnitude) == "255211775190703847514520607242133176326" &&
        loss.total == loss.unrealised, "full-width open FIFO PnL differs");
    const std::array closing{
        priced_trade(4, maximum, 1, 2, 1), priced_trade(5, maximum, 1, 2, 1), priced_trade(6, maximum, 1, 2, 1)};
    require(ledger.apply(closing).has_value(), "full-width FIFO closing failed");
    auto result = marketlab::fifo_pnl(ledger, {1}, Price::from_ticks(maximum).value()).value();
    require(result.realised == loss.unrealised && result.unrealised == PnlAmount{} && result.total == loss.total,
        "full-width realised FIFO PnL differs");
    const auto self = priced_trade(std::numeric_limits<std::uint64_t>::max(), maximum, maximum, 1, 1);
    require(ledger.apply(std::span{&self, 1}).has_value() &&
        marketlab::fifo_pnl(ledger, {1}, mark_100) == result, "final sequence self-match changed FIFO PnL");
    check_identity(result);
}

struct UnitOracle {
    std::vector<std::int64_t> longs;
    std::vector<std::int64_t> shorts;
    std::int64_t realised{};
    std::uint64_t bought{};
    std::uint64_t sold{};
    std::uint64_t spent{};
    std::uint64_t received{};

    void apply(bool buying, std::int64_t ticks, std::int64_t units) {
        (buying ? bought : sold) += static_cast<std::uint64_t>(units);
        (buying ? spent : received) += static_cast<std::uint64_t>(ticks * units);
        auto& opposite = buying ? shorts : longs;
        auto& same = buying ? longs : shorts;
        // Expand into individual units, independently of production's quantity lots.
        for (std::int64_t unit = 0; unit < units; ++unit) {
            if (opposite.empty()) {
                same.push_back(ticks);
            } else {
                realised += buying ? opposite.front() - ticks : ticks - opposite.front();
                opposite.erase(opposite.begin());
            }
        }
    }
};

void test_generated_unit_oracle() {
    ParticipantLedger ledger{{1}, participants};
    std::array<UnitOracle, 3> reference{};
    std::mt19937_64 random{1729};
    constexpr std::array<std::int64_t, 3> marks{1, 99, std::numeric_limits<std::int64_t>::max()};
    for (std::uint64_t sequence = 1; sequence <= 1000; ++sequence) {
        const auto buyer = random() % 3;
        const auto seller = random() % 3;
        const auto ticks = static_cast<std::int64_t>(random() % 250 + 1);
        const auto units = static_cast<std::int64_t>(random() % 5 + 1);
        const auto execution = priced_trade(sequence, units, ticks, buyer + 1, seller + 1);
        require(ledger.apply(std::span{&execution, 1}).has_value(), "generated FIFO ingestion failed");
        if (buyer != seller) {
            reference[buyer].apply(true, ticks, units);
            reference[seller].apply(false, ticks, units);
        }
        for (std::size_t index = 0; index < reference.size(); ++index) {
            const auto& expected = reference[index];
            std::uint64_t basis = 0;
            for (auto entry : expected.longs) basis += static_cast<std::uint64_t>(entry);
            for (auto entry : expected.shorts) basis += static_cast<std::uint64_t>(entry);
            for (const auto mark : marks) {
                const auto result = marketlab::fifo_pnl(ledger, {index + 1}, Price::from_ticks(mark).value()).value();
                const auto marked = decimal_product(expected.longs.size() + expected.shorts.size(), static_cast<std::uint64_t>(mark));
                const auto unrealised = expected.shorts.empty() ?
                    decimal_difference(marked, std::to_string(basis)) : decimal_difference(std::to_string(basis), marked);
                const auto credits = add_decimal(std::to_string(expected.received),
                    decimal_product(expected.bought, static_cast<std::uint64_t>(mark)));
                const auto debits = add_decimal(std::to_string(expected.spent),
                    decimal_product(expected.sold, static_cast<std::uint64_t>(mark)));
                const auto total = decimal_difference(credits, debits);
                require(result.realised == small_pnl(expected.realised) &&
                    result.unrealised.negative == unrealised.first &&
                    marketlab::to_string(result.unrealised.magnitude) == unrealised.second &&
                    result.total.negative == total.first && marketlab::to_string(result.total.magnitude) == total.second,
                    "generated FIFO result differs from unit-lot decimal oracle");
                check_identity(result);
            }
        }
    }
    ParticipantLedger rebuilt{{1}, participants};
    require(rebuilt.apply(ledger.history()).has_value(), "generated FIFO replay failed");
    for (const auto participant : participants) {
        require(marketlab::fifo_pnl(rebuilt, participant, mark_100) == marketlab::fifo_pnl(ledger, participant, mark_100),
            "generated FIFO replay projection differs");
    }
}

int main() {
    try {
        test_fifo_and_reversals();
        test_short_fifo_and_self_matches();
        test_three_participant_reconciliation();
        test_full_width();
        test_generated_unit_oracle();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
