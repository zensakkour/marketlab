# Multi-instrument exchange

`MultiInstrumentExchange` from `<marketlab/multi_instrument_exchange.hpp>`
owns one existing `Exchange` per configured instrument under one logical writer.
Each exchange retains its own book, accepted-order history, time, request/event
counters, last trade and cumulative volume. The matching implementation is shared;
mutable market state is not shared.

## Configuration and read access

Construct from `const InstrumentRegistry&` and a fixed
`span<const ParticipantId>`. Instruments and participant registration are copied
into the owned exchanges; the source registry and caller lists may then be
destroyed. Participant IDs must be nonzero even when the instrument list is empty.
Duplicate participant registrations name the same participant, as in `Exchange`.
An empty instrument list or empty participant set is valid.

The owner cannot be copied or moved. `size()`/`empty()` are non-allocating
queries. `find(InstrumentId) noexcept` returns a borrowed `const Exchange*`,
or nullptr for an unknown ID, including zero. Addresses remain valid until owner
destruction; processing requests does not add/remove instruments. Callers cannot
mutate exchanges or books through these views.

`Exchange::instrument() noexcept` returns a borrowed `const Instrument&`
to its immutable owned configuration. It preserves the original symbol and exact
tick parameters; no copy or allocation occurs. The reference lasts until exchange
destruction, including across successful and refused requests.

Use existing projections on a found exchange:
`best_prices(*view)`, `market_snapshot(*view)`, `depth_snapshot(*view, N)`.
Resting-order pointers still follow the existing book mutation lifetime rules;
a stable exchange pointer does not make its changing resting records immutable.

## Routing and failure contract

`process_order(OrderRequest)` and `process_cancel(CancelRequest)` return
`expected<vector<ExchangeEvent>, MultiInstrumentError>`.

- The request's numeric `instrument_id` chooses its exchange.
- An absent instrument returns `UnknownInstrument` without an exchange event,
  clock advance, assigned request/event sequence or mutation to any instrument.
  This routing check precedes all fields that belong to exchange validation.
- A known instrument delegates the complete raw request unchanged.
  Acceptance/rejection precedence, matching, FIFO, execution price, cancellation
  ownership and committed-rejection behavior are the existing exchange rules.
- Engine diagnostics map to `RequestSequenceExhausted`,
  `EventSequenceExhausted` or `OutputCapacityExceeded`, preserving the
  delegated exchange's no-mutation guarantee. They are not order rejections.
- Allocation exceptions propagate. Failed construction publishes no partial
  owner and leaves source configuration unchanged. Failed processing preserves
  both the target exchange and every other exchange, permitting an identical
  retry. Successful return moves the delegated event vector without allocation.

A known instrument with an invalid participant produces the normal exchange
rejection event. An unknown instrument produces a routing diagnostic even when
other raw fields are also invalid. This deliberately differs from submitting a
foreign-instrument request directly to a single `Exchange`, which has an owned
event stream in which to emit `InvalidInstrument`.

## Scope of time, sequences and identities

Each instrument has its own logical clock and monotonic request/event sequences.
There is no global router clock or event counter. For example, after A accepts
a request at t=10, B may accept t=1 if B's own clock permits it.
A request on A never advances B. Current/equal-time requests preserve call order
within their instrument. Calls are processed synchronously by the single writer.

Accepted-order lifetime uniqueness is per instrument. A and B may both accept
OrderId 1; a duplicate on A remains a duplicate on A after fill/cancel/expiry,
without reserving that ID on B. Clients may still use a globally coordinated ID
source if they prefer. Symbols do not route requests; InstrumentId does.

Consequently, event identity is **(InstrumentId, EventSequence)** and request
identity is **(InstrumentId, RequestSequence)**. A merged v1 recording preserves
caller processing order. Do not sort it by local sequence values or assume
globally increasing timestamps. See [recording scope](event-record-format.md#multi-instrument-streams).

The existing `Simulation` remains single-instrument. This router owns no queue,
scheduler or callbacks. A caller can drive it from an existing `SimulationQueue`
when one time-ordered arrival stream is wanted, as the example does. No threads,
locks or worker channels are introduced here.

## Accounting isolation

Keep one `ParticipantLedger` per instrument and route only that instrument's
committed executions into it. Existing ledger validation rejects foreign
executions without changing accounts/history. Per-instrument event ordering is
sufficient; equal local sequence numbers on different instruments do not conflict.

Cash, positions, fills and FIFO PnL remain instrument-local. Supply an explicit
valuation mark for each instrument. Raw tick notionals from instruments with
different tick sizes are not a portfolio cash amount and are not summed here.
No multi-currency/portfolio valuation convention is introduced.

## Runnable two-instrument scenario

`multi_instrument [--events]` is a fixed deterministic example:
ALPHA (ID1, tick0.01) and BETA (ID2, tick0.5), with participants1/2.
An existing arrival queue processes twelve requests in timestamp/insertion order.
Both instruments accept the same client IDs independently, trade, partially fill,
cancel and expire unfilled market quantities. One destination99 is unknown.

The example caller counts/discards that one routing diagnostic and consumes its
queue entry; it creates no exchange event. Engine diagnostics abort with the
arrival still pending. Accounting/output failures abort the local example;
whole-run checkpoint/retry is not promised.

Default output has seven lines: total arrivals/diagnostics/events, then one
market line and two participant account/fill/PnL lines for each instrument.
Results own configuration, snapshots and account/valuation data after the router
and ledgers are destroyed. `--events` writes only exchange records, including
their instrument-local counters. Exit codes: 0 success,1 run/output failure,
2 invalid arguments. The example has no random input or seed argument.

| Metric | ALPHA | BETA |
|---|---:|---:|
| Final time (ns) | 6 | 5 |
| Processed requests | 6 | 5 |
| Exchange events | 12 | 13 |
| Volume (units) | 3 | 7 |
| Last trade (ticks / units) | 100 / 3 | 200 / 3 |
| Explicit mark (ticks) | 101 | 201 |
| Participant1 position / cash | -3 / +300 | -7 / +1400 |
| Participant2 position / cash | +3 / -300 | +7 / -1400 |
| Participant1 / participant2 total PnL | -3 / +3 | -7 / +7 |

Both books finish empty. Cash/PnL are in each instrument's tick-times-quantity
units. There are 12 arrivals,1 routing diagnostic and25 exchange records.
The LF-normalized v1 event stream has FNV-1a-64 checksum
`9e74fbb840f4cf87`. See [README commands](../README.md#run-the-multi-instrument-scenario).

Tests compare every routed event and complete bounded-fixture book/market state
against two isolated exchanges after each request; they also check local clocks,
shared client IDs, rejection precedence, cancellation and independent ledgers.
Construction and processing allocation injection covers identical retry and
unchanged foreign state. The example's exact summary/checksum and twenty replays
cover cross-instrument equal-time arrivals and independently retained results.

This completes single-writer routing/isolation. Worker assignment and concurrent
delivery belong to Phase12; no multithreading or throughput result is claimed.
