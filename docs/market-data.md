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

## Current scope

This API publishes prices and committed-state metadata. It does not expose the
FIFO head's remaining quantity as a price-level total: those are different values.
Aggregated quantities need checked arithmetic for multiple maximum-size orders.
Spread/midpoint representation, depth, last trade and cumulative volume are
subsequent Phase 7 work.

`market_data` checks empty/one-sided/two-sided books, passive price changes,
same-price FIFO survivors, partial and full fills, multi-level fills, cancellation,
rejected requests, saved snapshots, signed price boundaries and scheduled delivery.
`exchange_property` compares the snapshot to its independent flat execution ledger
after every generated request. Allocation tests read empty and populated snapshots
with the next allocation forced to fail.

Build and run the tests using the [README](../README.md#build-run-and-test).
