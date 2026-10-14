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

## Current scope

This API publishes prices and committed-state metadata. It does not expose the
FIFO head's remaining quantity as a price-level total: those are different values.
Aggregated quantities need checked arithmetic for multiple maximum-size orders.
Depth, last trade and cumulative volume are subsequent Phase 7 work.

`market_data` checks empty/one-sided/two-sided books, passive price changes,
same-price FIFO survivors, partial and full fills, multi-level fills, cancellation,
rejected requests, saved snapshots, signed price boundaries and scheduled delivery.
Spread/midpoint checks cover odd/even spreads, half-ticks and sums beyond
INT64_MAX, including adjacent maximum prices.
`exchange_property` compares the snapshot to its independent flat execution ledger
after every generated request, including derived spread/midpoint values. Allocation
tests read empty and populated snapshots and compute derived values with the next
allocation forced to fail.

Build and run the tests using the [README](../README.md#build-run-and-test).
