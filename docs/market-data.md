# Market-data projection

Market data is a read-only projection of completed exchange state. The first
API is `best_prices(const Exchange&) noexcept` from `<marketlab/market_data.hpp>`.
It returns an owned `BestPrices` value, without allocating or changing a book,
clock, counter or event stream.

## Best-price snapshot

| Field | Meaning |
| --- | --- |
| `instrument_id` | The book's configured instrument, including after a rejected request for another instrument. |
| `timestamp` | The exchange's committed logical time. |
| `last_request_sequence` | Last processed request, including rejections; zero before any request. |
| `last_event_sequence` | Last produced event; zero before any request. |
| `bid` | Exact highest active bid price, or an empty optional when the bid side is empty. |
| `ask` | Exact lowest active ask price, or an empty optional when the ask side is empty. |

Prices retain the positive signed 64-bit tick representation. The snapshot copies
only price values, not borrowed resting-order pointers. A previously returned
snapshot remains unchanged after fills, cancellation or destruction of source
records. Consumers read a new snapshot to observe later processing.

Call under the same single logical writer, after a complete order/cancel request
or simulation step returns. The snapshot is not a synchronization mechanism for
concurrent mutation. Pending scheduled arrivals do not change its values or
metadata before they reach the exchange. Empty simulation steps produce no new
state. Non-backward rejected requests update snapshot time/counters according to
the exchange contract while leaving prices unchanged.

## Spread and exact midpoint

`BestPrices::spread_ticks()` returns `ask - bid` as signed integer ticks.
`BestPrices::mid_price()` returns an optional `MidPrice` with `whole_ticks` and
`half_tick`: the whole tick count plus one half-tick when the flag is true.
Both methods return an empty optional if either side is absent. Neither rounds
nor uses floating point; both are computed from the snapshot's owned prices.

For example, bid 100 / ask 101 has spread 1 and midpoint `{100, true}` (100.5
ticks). Bid 100 / ask 102 has spread 2 and midpoint `{101, false}` (101 ticks).
Completed v0 books are strictly uncrossed, so a two-sided spread is positive.
These are observations of completed exchange state, not a new order-price type.

The price difference fits in signed 64 bits because both positive prices are
bounded by INT64_MAX. Midpoint calculation converts each positive price to
unsigned 64 bits before adding; the largest possible sum is 2 * INT64_MAX, which
fits without wrap. Halving returns an in-range signed whole tick value and
preserves the low bit as the half-tick flag. Derived values are not cached and
remain tied to the same saved price values after later exchange processing.

```cpp
const auto quote = marketlab::best_prices(simulation.exchange());
const auto spread = quote.spread_ticks();
const auto mid = quote.mid_price();
// Check the optionals before reading mid->whole_ticks and mid->half_tick.
```

## Last trade and combined snapshot

`Exchange::last_trade() noexcept` returns an owned optional `Execution`.
It is empty before the first execution. After a request that executes, it retains
the final execution in that request's committed event sequence. Multiple fills
at one timestamp therefore retain event ordering, rather than choosing by time,
price, order ID or quantity. Both aggressive sides and permitted self-matches
use the existing execution contract.

Passive orders, cancellations, rejections and markets with no fills preserve
the last trade. A market remainder expiry after fills also preserves the final
execution. Engine errors and allocation exceptions preserve the previously
committed trade along with the book, clock and counters.

The exchange owns this derived record because completed fills may remove their
source orders. It copies from its validated execution output after book processing
succeeds; the copy allocates nothing and cannot throw. There is no public setter,
separate feed or independently writable market state.

`market_snapshot(exchange) noexcept` returns `MarketSnapshot{prices, last_trade}`.
The `prices` member is the same owned `BestPrices` projection described above.
Both members are read at one completed request boundary under the single writer.
Snapshot metadata describes current committed processing; the execution retains
its own original trade timestamp and request/event sequences. An empty book can
still have a last trade. All returned values survive source mutation/destruction.

```cpp
const auto market = marketlab::market_snapshot(simulation.exchange());
if (market.last_trade) {
    const auto ticks = market.last_trade->price.ticks();
    const auto units = market.last_trade->quantity.units();
    // Consume this execution's exact price and size.
}
```

## Current scope

These APIs publish prices, last execution and committed-state metadata. They do
not expose the
FIFO head's remaining quantity as a price-level total: those are different values.
Aggregated quantities need checked arithmetic for multiple maximum-size orders.
Depth and cumulative volume remain subsequent Phase 7 work.

`market_data` checks empty/one-sided/two-sided books, passive price changes,
same-price FIFO survivors, partial and full fills, multi-level fills, cancellation,
rejected requests, saved snapshots, signed price boundaries and scheduled delivery.
Spread/midpoint checks cover odd/even spreads, half-ticks and sums beyond
INT64_MAX, including adjacent maximum prices.
`exchange_property` compares the snapshot to its independent flat execution ledger
after every generated request, including derived spread/midpoint values. Allocation
tests read empty and populated snapshots and compute derived values with the next
allocation forced to fail.

Last-trade checks cover multi-level fills, both sides, self-matches, exact maximum
price/size, equal-time executions, retained values after source destruction,
non-trading requests, scheduled delivery and allocation-failure retry. Generated
workloads compare every last-trade field to the reconciled execution ledger.
The scripted CLI exposes six tested observations; see the README's
[market snapshot example](../README.md#inspect-the-scripted-market-snapshots).

Build and run the tests using the [README](../README.md#build-run-and-test).
