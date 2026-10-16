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

`market_snapshot(exchange) noexcept` returns
`MarketSnapshot{prices, last_trade, cumulative_volume}`.
The `prices` member is the same owned `BestPrices` projection described above.
All members are read at one completed request boundary under the single writer.
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

## Exact aggregate quantities

Individual matching quantities remain nonnegative signed 64-bit `Quantity`
values. A level or lifetime total can exceed both INT64_MAX and UINT64_MAX.
`AggregateQuantity` from `<marketlab/aggregate_quantity.hpp>` represents exact
nonnegative units as `high_units * 2^64 + low_units`, using two unsigned 64-bit
words and standard C++23 arithmetic. Every word pair is valid; comparisons order
the high word first.

`add(Quantity)` returns a new optional aggregate. It detects a carry from the low
word and returns an empty optional if the high word would overflow, leaving the
source unchanged. Zero additions are valid. `marketlab::to_string(aggregate)`
returns exact decimal digits, including zero, without floating point or locale
formatting. String construction can allocate; the arithmetic itself is constexpr,
non-allocating and noexcept.

Two words suffice for all valid v0 state. The exchange processes at most
UINT64_MAX requests, and each incoming order executes at most INT64_MAX units.
Cumulative volume is therefore less than 2^127. Likewise, at most UINT64_MAX
distinct nonzero order IDs can be active, each with a remainder at most INT64_MAX,
so any level total is less than 2^127. Assertions check these internal invariants;
valid exchange/book state never exhausts the aggregate representation. No new
matching rejection, saturation, rounding or dropped observation is introduced.

## Cumulative traded volume

`Exchange::cumulative_volume() noexcept` returns the exact total executed units
since exchange construction, initially zero. `MarketSnapshot::cumulative_volume`
copies the same value. Each execution contributes its quantity once, including
a permitted self-match; it is not counted once per counterparty.

Successful order processing adds original quantity minus the unfilled remainder,
which equals the sum of its executions. A limit's rested remainder and a market's
expired remainder are excluded. Passive orders, zero-fill markets, rejections,
cancellations and pending arrivals add nothing. Allocation/engine failure retains
the old volume; a successful retry adds the fills once. The non-allocating update
shares the exchange's completed request boundary with last trade and metadata.
There is no public setter or independent volume feed.

## Top-N depth

`OrderBook::depth(N)` returns an owned `BookDepth` with separate `bids` and
`asks` vectors. Each contains at most N distinct active price levels, best first:
descending bids and ascending asks. N applies separately to each side. Empty
sides produce empty vectors; N=0 produces two empty vectors. Asking for more
levels than exist returns only those available, without reserving N entries.

Each `DepthLevel` contains:

| Field | Meaning |
| --- | --- |
| `price` | Exact matching price of the level. |
| `quantity` | Exact `AggregateQuantity` sum of every active order's remaining units at that price. |
| `order_count` | Number of active resting orders at that price. |

A partial fill reduces the quantity but retains its order count; a full fill or
cancellation removes that order's remainder and reduces the count. Empty levels
are absent. The FIFO head's size and original submitted sizes are not level totals.
The query reads current book records directly and maintains no mutable depth cache.

`depth_snapshot(exchange, N)` returns `DepthSnapshot{market, depth}`, combining
the complete `MarketSnapshot` with the same owned book-depth projection. Even
N=0 retains committed metadata, last trade and cumulative volume. Read only at a
completed request/step boundary under the single writer. All copied values survive
source mutation and destruction.

Depth allocates vectors proportional to the returned level count and visits the
orders at those levels. Unlike `market_snapshot()`, it is not noexcept. Allocation
or vector-capacity exceptions propagate; no partial result is returned and book,
exchange state, counters and existing borrowed views remain unchanged. Calling
again is safe. Reads do not assign sequences or consume pending arrivals.

```cpp
const auto snapshot = marketlab::depth_snapshot(simulation.exchange(), 5);
const auto volume = marketlab::to_string(snapshot.market.cumulative_volume);
for (const auto& bid : snapshot.depth.bids) {
    const auto units = marketlab::to_string(bid.quantity);
    // Consume bid.price, exact units and bid.order_count.
}
```

## Validation

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

Depth and volume checks cover zero/one/multiple requested levels, asymmetric
sides, ordering, same-price aggregation, counts after partial/full fills and
cancellation, crossing remainders, empty markets, scheduling and saved values.
Three maximum-size orders at one price and three maximum-size executions verify
exact totals above UINT64_MAX. Allocation injection covers each depth vector
allocation and order/cancel/delivery failures, followed by successful retry.
The generated ledger uses independent decimal addition for both level quantities
and cumulative executed volume after every request, with varying depth limits.
`aggregate_quantity` also checks 128-bit decimal boundaries, checked overflow,
constexpr arithmetic and 10,000 generated additions against that decimal oracle.

Build and run the tests using the [README](../README.md#build-run-and-test).
