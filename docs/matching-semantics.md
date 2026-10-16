# MarketLab v0 matching semantics

This is the working contract for the single-instrument core. It defines behavior
before data structures or matching code are chosen. v0 has one logical writer,
continuous trading, limit orders, market orders, and full cancellation of an
order's remaining quantity. Amendments, auctions, additional time-in-force
policies, fees, and pre-trade position limits are outside v0.

## 1. Identifiers

Order, participant, and instrument IDs are unsigned 64-bit integers. Zero is
invalid; the maximum value is valid. Every order belongs to one configured
participant and the one configured instrument. Configuration is fixed during a
run. Unknown participant or instrument IDs are rejected before book mutation.

Instrument configuration requires a nonzero ID, a nonempty symbol, and a valid
tick size. The symbol is a label; routing and request validation use instrument
IDs. Symbols are retained as supplied without case conversion or trimming.

The v0 domain represents participants through `ParticipantId`. A separate
participant object is not required to identify order owners or execution
counterparties. Registration belongs to exchange configuration; participant
behavior and execution-based accounting are later components.

The submitting participant supplies an order ID. Accepted order IDs are unique
across the exchange instance for its entire lifetime, including filled,
cancelled, and expired orders. A duplicate is rejected without affecting the
original order. A rejected submission does not reserve its ID. Creating a new
exchange instance starts a new ID namespace.

## 2. Price

Matching-core prices are signed 64-bit integer tick counts (`std::int64_t`).
Valid prices are in the inclusive range 1 through 9,223,372,036,854,775,807 ticks.
Zero, negative values, and values outside that range are rejected.

Instrument tick size is an exact decimal: a positive signed 64-bit coefficient
and an unsigned scale from 0 through 18. Its value is coefficient / 10^scale.
For example, coefficient 1 and scale 2 means 0.01; coefficient 5 and scale 2
means 0.05. Invalid tick configuration prevents exchange initialization.

The decimal input adapter accepts text consisting of one or more ASCII digits,
optionally followed by a decimal point and one or more ASCII digits. Leading
and trailing zero digits are allowed. Signs, whitespace, exponent notation,
commas, and missing digits are invalid. Conversion uses exact integer
arithmetic, never floating point. The matching core accepts ticks directly;
the decimal adapter is a separate boundary and is not required for the first
book implementation.

A human decimal price is valid only if it is an exact multiple of the
instrument's tick size and the resulting tick count is in range. Off-tick and
out-of-range inputs are rejected rather than rounded or truncated.
With tick size 0.01, 100.01 and 100.010 both mean 10,001 ticks; 100.005 is
rejected. With tick size 0.05, 100.05 means 2,001 ticks and 100.01 is rejected.

## 3. Quantity

Quantities use signed 64-bit integer units. Submitted quantity must be from 1
through 9,223,372,036,854,775,807 inclusive. Zero, negative values, fractional
units, and out-of-range values are rejected. v0 uses lot size one and has no
additional configurable quantity cap. Remaining quantity may be zero only for
an order that is no longer resting. Subtraction must never produce a negative
quantity; accounting and aggregate arithmetic must not silently overflow.

## 4. Priority and logical time

Higher bids and lower asks have better priority. Orders at the same price
execute in the order in which the exchange accepted them. An order's remaining
quantity retains its original priority after a partial fill.

The single writer processes requests serially. It assigns each request a unique
increasing unsigned 64-bit sequence, starting at one, in entry-point call order.
This sequence, not order ID or participant ID, establishes arrival order even
when logical timestamps are equal. Rejected requests also receive a sequence.

`RequestSequence` and `EventSequence` are distinct unsigned 64-bit types; zero
is invalid for an assigned sequence. An `AcceptedOrder` retains its original
quantity, acceptance time, and arrival request sequence. A `RestingOrder` pairs
that record with a positive remaining quantity no greater than the original;
only limit orders can rest. These records check intrinsic consistency through
`is_valid()`; acceptance and book membership require authoritative exchange state.

Logical timestamps are unsigned 64-bit nanoseconds from the simulation origin;
zero is valid. Requests are processed with nondecreasing timestamps. A backward
timestamp is rejected without advancing logical time. v0 allows every request
to use timestamp zero. Simulation scheduling preserves this ordering contract.
Wall-clock time and thread scheduling never determine priority.

### Scheduled arrival records

`SimulationEvent` represents a scheduled order or cancel arrival. Its request
timestamp is the scheduled exchange-arrival time, and its `SimulationSequence`
is distinct from exchange request/event sequences. `SimulationQueue` assigns
unique positive unsigned 64-bit sequences in scheduling insertion order, starting
at one, without wrapping. Records order by arrival time first, then this sequence.
At equal times, the request added to the schedule first reaches the exchange first,
regardless of order ID, participant ID, or whether it is an order or cancellation.
The exchange assigns its own request sequence when it processes the arrival.

`is_valid(SimulationEvent)` checks assigned simulation identity and payload
presence; it deliberately permits malformed request fields for exchange rejection.
`scheduled_before()` compares scheduling keys only.

`SimulationQueue` owns the pending arrivals and assigns their sequence on successful
`schedule(SimulationRequest)` calls. Callers supply raw requests, not scheduling
identities. A failed insertion consumes no sequence and preserves the existing
queue. Sequence exhaustion or the vector storage maximum returns an explicit
`SimulationQueueError`; allocation exceptions propagate without changing pending
records, ordering, the counter or borrowed views. Nonthrowing payload moves and
heap comparisons permit sequence commit only after storage succeeds.

`next()` is a const borrowed view of the earliest arrival; reacquire it after a
successful mutation. `pop_next()` transfers the earliest record without allocation
and returns an empty optional when no arrival remains. Emptying the queue does not
reset its counter. Queue copies/moves are disabled to keep ownership explicit.

`Simulation` owns an exchange and its queue with one logical writer. Construction
starts at logical time zero. `schedule()` accepts current/future arrival times and
refuses the past before queue mutation, without an exchange event or sequence
consumption. Other malformed request fields are preserved for exchange rejection.
Scheduling sequence/storage errors are explicit; allocation exceptions leave
simulation state unchanged.

`step()` delivers the earliest arrival using the existing exchange entry points.
A `SimulationStep` owns that arrival and its returned events. An empty optional
means the queue is drained, while an expected error reports an `ExchangeError`.
A committed order/cancel rejection also removes its arrival and advances time
according to the exchange contract. Engine errors and allocation exceptions keep
the head pending and preserve committed time, book, identity history and counters;
the caller stops on an engine error. Removal and result transfer after commitment
are nonthrowing and allocation-free.

The exchange clock is authoritative; the runner maintains no duplicate clock.
Empty steps do not advance time. A drained simulation may resume with new arrivals
at the current or a future timestamp; all counters retain their history. Only
const exchange/queue views are exposed. No callback interleaves with request
processing. Participant actions and delayed report/market-data delivery remain
later work.

`tests/simulation_delivery_tests.cpp` verifies the scripted bid, ask, partial
execution and cancellations through `Simulation::step()`. It checks chronological
arrival, equal-time insertion FIFO, complete exchange events, intermediate
heads/invariants, clock/counters and final active records. Simulation identities
differ from request sequences. Twenty fresh replays reproduce the complete
event/execution sequence and final book. Lifecycle tests cover initial/empty
steps, committed rejections, refusal of past requests, equal-time restart and
maximum logical time.
Allocation tests cover retained arrivals and unchanged exchange state followed
by identical retry for limit/market matching, owned cancellation and both kinds
of rejection, with later arrivals still present.

## 5. Limit orders

A buy limit crosses asks at prices less than or equal to its limit; a sell
limit crosses bids at prices greater than or equal to its limit. Each match
uses the best eligible opposite price, then FIFO within that price level.
Execution quantity is the smaller of the two remaining quantities.

Every execution uses the resting order's price, including when an aggressive
order walks several levels. Matching continues until the incoming order is
filled or no eligible opposite order remains. Any positive remainder rests at
the incoming limit price, with the incoming order's acceptance priority.
A non-crossing limit rests immediately. Resting limits remain until filled or
cancelled. The book must not remain crossed after processing a request.

v0 permits matches between orders owned by the same participant. They use the
same priority and price rules and produce the same execution records. Accounting
must apply both sides of such an execution, resulting in no net change to that
participant's position or cash. Self-trade prevention is not implemented in v0.

## 6. Market orders

A market order has no limit price. It consumes opposite liquidity in best-price
and FIFO order, using resting prices, until filled or that side becomes empty.
A market order never rests. Any positive remainder expires immediately,
including the entire quantity when the opposite side is empty. An otherwise
valid market order is accepted even when no liquidity is available.

## 7. Partial fills

Each execution subtracts the executed quantity from both orders' remaining
quantities. A partially filled resting order stays at its existing position in
the price-level queue. A fully filled resting order is removed from the book
and active-order index, and an empty price level is removed.

A partially filled incoming order continues matching before resting or expiring
its remainder. No participant callback or later request may interleave with
processing one request. Execution records, rather than fill-status events,
are the source of truth for accounting.

## 8. Cancellation

A cancellation names the requesting participant, instrument, and target order
ID. It removes all remaining quantity of an active order owned by that
participant. A partially filled order can be cancelled; prior executions are
unchanged. Successful cancellation removes the order from the active index and
removes its price level if empty. It emits the removed remaining quantity.

An unknown, filled, cancelled, or expired order produces `CancelRejected` with
reason `NotActive`. v0 does not distinguish these cases through cancellation.
An active order belonging to another participant produces `NotOwner`.
No failed cancellation changes book state or priority. Cancellation never
releases an accepted order ID for reuse.

## 9. Validation and rejections

Validate the whole request before matching or storing any order. The first
failure in the following order determines a new-order rejection reason:

1. `InvalidTimestamp`: timestamp precedes current logical time.
2. `InvalidInstrument`: zero or unknown instrument ID.
3. `InvalidParticipant`: zero or unknown participant ID.
4. `InvalidOrderId`: zero order ID.
5. `InvalidSide`: side is not buy or sell.
6. `InvalidOrderType`: type is not limit or market.
7. `InvalidQuantity`: submitted quantity is outside the supported range.
8. `InvalidPrice`: limit price is missing/invalid, or a market order carries a price.
9. `DuplicateOrderId`: the ID belongs to any previously accepted order.

Core request data retains raw signed 64-bit quantity units and optional tick
prices until validation, so zero or negative submissions can be rejected with
their supplied values preserved. A limit requires a price; a market requires
that the price field be absent, including when the supplied value would be zero.
The `validate_fields()` helper checks only intrinsic request fields. Passing it
does not establish registration, timestamp ordering, duplicate-ID status,
active-order status, or cancellation ownership, and does not accept an order.

An adapter must reject malformed or out-of-range numeric input before creating
an internal request. Adapter parsing failure emits no core exchange event.
A request rejected by the exchange emits exactly one `OrderRejected` event.
It reserves no order ID, changes no resting order or priority, and produces no
execution. A non-backward request advances logical time even if rejected.

Cancellation validates timestamp, instrument, participant, and nonzero target
ID in that order, using the same reasons. It then checks active status before
ownership. A validation or lookup failure emits exactly one `CancelRejected`.

Request/event counters must never wrap. The engine must stop with an explicit
capacity error before processing a request that would exhaust either counter;
it must not partially process that request. Counter exhaustion is an engine
failure, not an order rejection.

## 10. Exchange events

Each event carries its logical timestamp, unique increasing unsigned 64-bit
event sequence (starting at one), originating request sequence, instrument ID,
and relevant participant/order IDs. Rejections preserve the supplied IDs and
include the reason. A backward-timestamp rejection is stamped with current
logical time and also records the rejected request's timestamp.

For an accepted new order, events occur in this order:

1. `OrderAccepted`: ID, owner, side, type, original quantity, and optional limit price.
2. For each match, `Execution`: aggressive and resting IDs, buyer and seller IDs,
   aggressive side, execution price, and executed quantity. The event sequence
   identifies the execution uniquely. Each execution is reported once, not once per side.
3. Immediately after each execution, the resting order's `OrderFilled` if its
   remaining quantity is zero, otherwise `OrderPartiallyFilled` with its remainder.
4. Then the incoming order's `OrderFilled` or `OrderPartiallyFilled` on the same
   basis. Status events carry the order ID, owner, and remaining quantity.
5. After matching, `OrderRested` with price and remaining quantity for a positive
   limit remainder, or `MarketRemainderExpired` with remaining quantity for a
   positive market remainder. A fully filled order emits neither event.

A zero-execution market order emits `OrderAccepted`, then
`MarketRemainderExpired`. Successful cancellation emits only `OrderCancelled`,
with order ID, owner, price, and cancelled remaining quantity. Failure emits
only `CancelRejected`. Rejected new orders emit only `OrderRejected`.
No additional per-side execution or market-data events are emitted by the core.

The `Execution` record represents one trade without a separate duplicate trade
record. Its aggressive side identifies which order is the buy: the aggressive
order for `Buy`, or the resting order for `Sell`. The two order IDs must differ;
buyer and seller participant IDs may be equal. `is_valid()` checks nonzero IDs
and sequences, valid side, and positive quantity. The matching core must establish
counterparty ownership, execution price, and available quantities from its orders.

`ExchangeEvent` holds one of the nine event records in a `std::variant`. The
non-execution records share an `EventHeader`; `Execution` already carries those
metadata fields directly. An acceptance header must agree with its accepted
order's instrument, acceptance time, and arrival sequence. Partial-fill, rested,
expired, and cancelled quantities are positive; a full-fill remainder is zero.
Successful records require nonzero numeric instrument, order, and participant IDs.

Rejection records retain the entire original request and a reason valid for that
request kind. Their header instrument matches the submitted instrument, even
when zero. An `InvalidTimestamp` record requires an earlier submitted time than
the event time; other rejection timestamps equal the submitted time because
non-backward requests advance logical time. Record checks do not consult exchange
state or establish rejection precedence, registration, ownership, quantity
transitions, or the ordering/uniqueness of a stream. Those remain matching-core
obligations.

## 11. Authoritative state

The order book owns authoritative resting-order state for its instrument,
including remaining quantities and priority. One logical writer processes all
changes to that book; v0 processes one instrument without concurrent mutation.
The exchange retains accepted IDs for duplicate checking independently of
whether those orders remain active.

Participant positions and cash change only through executions. Order submission,
acceptance, resting, and cancellation do not change filled positions or cash.

Market data is derived from exchange state. Quotes and depth reflect the order
book; trade information reflects executions. Market data must not become an
independently mutated source of market state.

The first implemented projection, `best_prices(exchange)`, copies the configured
instrument, committed clock/counters and optional highest-bid/lowest-ask prices.
It reads completed exchange state without allocation, events or mutation. Returned
values retain their own prices after source records are removed. Pending arrivals
remain outside the snapshot until processed. Field definitions and current scope
are in [market-data.md](market-data.md).

The snapshot derives `spread_ticks()` as ask minus bid only when both prices
exist. `mid_price()` preserves the exact average as whole ticks plus a half-tick
flag, without rounding or floating point. Both are absent for an empty side.
Unsigned price summation prevents signed overflow at the price maximum; the
result remains an observation and does not change permitted order-price ticks.

`Exchange::last_trade()` copies the final committed `Execution` in event-sequence
order, or an empty optional before any execution. Non-trading requests retain it;
engine/allocation failures preserve it. `market_snapshot(exchange)` combines that
owned record with `BestPrices` at a completed request boundary. Snapshot time can
advance beyond the last trade's original time. The derived record has no public
mutation path and is updated only from successful exchange executions.

Cumulative volume adds executed units once per execution, including self-matches,
and excludes rested/expired remainders. It starts at zero, survives non-trading
requests unchanged and shares the exchange's failure-atomic commit boundary.
`AggregateQuantity` stores exact unsigned 128-bit totals using two 64-bit words;
v0 request and active-order bounds keep volume and any level total below 2^127.
`OrderBook::depth(N)` aggregates current remainders/counts at up to N prices per
side, best first. `depth_snapshot(exchange, N)` owns those levels and the complete
market snapshot. Query allocation failures change no state. Full definitions and
bounds are in [market-data.md](market-data.md).

### Current book primitives

`OrderBook` owns resting records for one instrument using ordered price levels
and FIFO queues, with a separate set of active order IDs. It is noncopyable and
nonmovable. `insert_passive()` takes an intrinsically valid resting limit for its
instrument, with an unused active ID and a request sequence greater than the last
successful passive insertion, complete order operation, or cancellation.
Sequence gaps are allowed; failed insertions reserve no ID and do not advance
arrival priority. The owning exchange supplies lifetime accepted-ID history,
registration, and logical-clock validation for raw requests.

Passive insertion cannot leave a crossed book. `WouldCross` reports that the
operation requires matching; it is not a new exchange rejection reason. Allocation
failures propagate after rolling back storage changes. No events, executions,
accounting changes, or cancellation are produced by this storage operation.

`best_bid()` and `best_ask()` borrow read-only FIFO-head records from the highest
bid or lowest ask level, returning null for empty sides. Callers must reacquire
these pointers after mutation. Price levels contain individual quantities rather
than an unchecked signed aggregate that could overflow with several large orders.

`find(OrderId)` borrows a read-only active resting record, or returns null for zero
or absent IDs. It resolves both sides, every price level, and non-head FIFO entries,
with current remaining quantity and original acceptance metadata. Partial fills
remain discoverable; completed orders disappear. Callers must reacquire views
after mutation. Lookup rejects absent IDs through the active-ID set and scans
owned queues for present IDs (linear in active orders), without allocation or
state changes. No pointer index is maintained. This query neither validates a
cancellation request nor checks its ownership; the separate `cancel()` operation
follows section 8 as described below.

`check_invariants()` performs a const structural diagnostic at completed operation
boundaries. Each level must be nonempty; each queued record must be an intrinsically
valid resting limit for the book instrument, with its side and original limit
matching the containing price level. Arrival request sequences are strictly
increasing within each FIFO, globally unique among active orders, and no greater
than the last successful book operation sequence. Active IDs are unique across
both sides and correspond exactly to the active-ID set. A bid must be strictly
below an ask whenever both sides are present. Ordered price maps establish level
ordering; partial fills retain the original arrival metadata.

The diagnostic changes no book state and uses temporary ID/sequence sets with
O(N log N) work and O(N) memory. Allocation exceptions propagate and leave all
book records, index entries, priority and borrowed views unchanged. Invoke it
explicitly after a book operation returns; intermediate remainder preparation is
private to processing. Exchange registration, clock and lifetime identity history
remain owned by the exchange; generated-workload tests independently reconcile
executions against their own active-order ledger.

`match_one()` consumes at most one eligible opposite FIFO head for an accepted
limit order and a positive supplied remainder no greater than the original quantity.
It validates record shape, instrument, absence of an active incoming ID, arrival
sequence greater than the last successful passive insertion, complete order
operation, or cancellation, and a nonzero caller-supplied execution sequence.
Its `MatchError` values are operation diagnostics, not exchange rejection reasons. Errors and absence of eligible liquidity leave the book unchanged.

A match returns one `Execution` and both remaining quantities without modifying
the incoming record. It uses the resting price and the minimum of the supplied
incoming remainder and the current resting remainder. The execution carries the
incoming acceptance time/request sequence, supplied event sequence, both order IDs,
and buyer/seller identities mapped from their actual sides. Partial resting fills
retain their acceptance time, original quantity, and queue position. Full fills
remove the active ID and FIFO head, and remove the level when empty. The operation
allocates no memory.

`match_count()` is a read-only preflight for a positive accepted limit remainder.
It shares the input checks of `match_one()`, excluding execution-sequence validation
because it does not produce an execution. It counts eligible resting orders in
best-price/FIFO order until the supplied remainder is exhausted or no eligible
liquidity remains. An order partially consumed by the incoming remainder counts
as one execution. It uses stored remaining quantities, not original quantities,
and subtracts each consumed remainder instead of accumulating an overflow-prone
liquidity sum. The count is bounded by the number of active orders.

Counting allocates no memory and changes neither records, active IDs, nor insertion
priority. It does not reserve event capacity or assign counters. The single writer
must leave the book unchanged between preflight and the corresponding matching;
callers coordinating individual primitives still need to reserve output and any
passive-remainder storage before mutation to prevent partial command application
on failure.

`match_one()` does not allocate sequences or advance book arrival priority; a
subsequent passive remainder keeps its original incoming sequence. The single
writer must finish that matching and resting before another request interleaves.

`process_order()` processes one complete trusted accepted limit or market. Its
`OrderResult` owns the ordered events and the final unfilled quantity. It validates
the record and book-level input conditions, previews the exact match count and final
remainder, and checks output-size arithmetic and the contiguous event-sequence
range starting at the caller-supplied positive sequence. Zero produces
`InvalidEventSequence`; a range that would wrap produces `EventSequenceExhausted`.
Unrepresentable event counts or vector capacity produce `OutputCapacityExceeded`.
These are operation/engine diagnostics, not exchange rejection reasons. Exact
capacity ending at the maximum sequence is usable; the caller must prevent any
further allocation from wrapping.

The operation reserves the complete event vector, then stores any positive
incoming limit remainder before consuming opposite liquidity. Private storage
preparation may briefly cross the book inside the call; one writer, no callbacks,
and no concurrent observers keep that state unobservable. Storage allocation failures
roll back before any execution occurs. After preparation, fills and event
construction allocate no memory and use nonthrowing value records. Matching uses
the same resting-price/minimum-quantity primitive as `match_one()`. Return events
follow section 10 exactly. Positive limit remainders retain the original limit,
quantity, acceptance time, and arrival priority. Markets consume all opposite
prices in best-price/FIFO order, never allocate resting storage, and emit `MarketRemainderExpired` for any
positive remainder. A zero-execution market produces exactly acceptance and expiry.
Fully filled incoming orders produce neither resting nor expiry.
Successful processing advances book arrival priority, including fully filled
orders. Errors and allocation failures leave book state and arrival priority
unchanged; allocation exceptions propagate.

The caller establishes exchange acceptance, clock/registration/lifetime-ID
validity, request-sequence assignment, and global event-counter uniqueness.
`OrderBook::process_order()` neither accepts raw requests nor emits rejections.
`Exchange::process_order()` provides that boundary for new orders; returned events
can be recorded using the exact text writer described in
[event-record-format.md](event-record-format.md).

`cancel()` takes a `CancelRequest` and caller-assigned request/event sequences,
returning `std::expected<OrderCancelled, CancelError>`. It checks the instrument
against the book (including zero), nonzero participant/target IDs, positive request
sequence greater than the last successful book operation, positive event sequence,
active lookup, and ownership, in that order. `NotActive` precedes `NotOwner`.
Clock ordering, configured participant registration, and global counter capacity
are caller prerequisites; this primitive does not select raw exchange rejections.

On success it captures the cancellation record before erasing the resting entry,
removes that active ID and any empty price level, and advances book arrival
priority. Removing a FIFO entry preserves survivor order and original metadata.
The record reports only the unfilled remainder at the original limit price,
stamped with request time and supplied sequences. Cancellation allocates no memory;
value assignment and return construction are checked as nonthrowing. Callers must
reacquire all borrowed views after mutation, including views of other queue entries.

All diagnostics leave state and book arrival priority unchanged. A single assigned
request/event sequence equal to the unsigned maximum is usable without increment
or wrap; subsequent global counter management remains with the exchange. The
exchange must retain lifetime accepted-ID history and emit `CancelRejected` when
appropriate. The primitive returns one successful record or a diagnostic, with
no executions, accounting changes, or rejection-event production.

### Current raw request entry points

`Exchange` owns one intrinsically valid `Instrument`, fixed registered participant
IDs copied from a span, a private book, lifetime accepted IDs, current logical time,
and last-assigned request/event sequences. Zero participant registrations fail
construction; repeated registrations refer to one participant, and an empty set
accepts no orders. Configuration cannot be changed after construction. The exchange
is noncopyable/nonmovable and exposes only a const borrowed book view.

`Exchange::process_order(OrderRequest)` returns an expected vector of exchange
events or `ExchangeError`. It selects the first rejection from section 9,
including timestamp/registration checks before intrinsic fields and lifetime
duplicates last. Rejections retain the complete supplied request. Non-backward
requests advance time even if rejected; backward requests emit at existing current
time. Both accepted and rejected requests receive one unique request sequence.
Event sequences remain contiguous across calls, including single-event rejections.
Counters start unassigned at zero; first assigned values are one. The largest
unsigned value is usable, and a further assignment fails before incrementing.

Accepted orders delegate matching/event construction to the owned book. Lifetime
ID storage is prepared before that call and rolled back on any book diagnostic or
allocation exception. Event output and passive storage preparation precede fills.
Clock/counters commit only once the entire result is ready; event-vector return is
nonthrowing. Filled and expired IDs remain reserved; rejection reserves none.
An identical retry after allocation failure sees unchanged book, history, time,
and counters. There are no callbacks or interleaved requests inside processing.

`RequestSequenceExhausted`, `EventSequenceExhausted`, and
`OutputCapacityExceeded` are engine capacity errors, not exchange rejections;
no request, event, time advancement, or state mutation is committed. The caller
must stop the run on capacity failure. Allocation exceptions propagate. A book
input diagnostic after validated exchange acceptance indicates an internal
invariant failure and throws `std::logic_error` without committing the request.
`current_time()`, `last_request_sequence()`, and `last_event_sequence()` report
committed state. `Simulation` owns scheduled request delivery. Exact text output
of returned events is documented in [event-record-format.md](event-record-format.md);
it preserves event order and does not mutate exchange or simulation state.

`Exchange::process_cancel(CancelRequest)` returns the same expected vector type,
containing exactly one `OrderCancelled` or `CancelRejected` event. It shares
request/event counters and logical time with new-order processing. Timestamp,
instrument registration, participant registration, and nonzero target ID checks
precede the book's active-status and ownership checks, in section 9 order.
Inactive targets emit `NotActive` before ownership is considered; a different
registered participant's active target emits `NotOwner`. Cancel rejections retain
the entire submitted request and have the same clock behavior as new-order
rejections. Successful cancellation reports the original limit and current
remainder, preserves survivor FIFO/metadata and all prior executions, and never
releases the target's lifetime accepted ID.

The entry point reserves its one-event output before calling the book's
nonallocating cancellation. After removal, event construction and result return
are checked as nonthrowing. Allocation failure commits no removal, lifetime-ID
change, clock advancement, or sequence assignment, so identical retry is valid.
Book input diagnostics beyond `NotActive/NotOwner` indicate internal invariant
failure and throw `std::logic_error` before request commitment. Capacity failures
use the same engine errors and stop-run requirement as new orders.

## 12. Canonical examples

Each row is an independent scenario for configured instrument 1. Prices are
ticks and quantities are units. Resting orders belong to participant 1; incoming
order `I` belongs to participant 2. Cancels are sent by participant 1 unless
specified. All requests use logical time zero and fresh IDs except duplicate
cases. Listed same-price orders are in FIFO order, left to right.
`B[...]` means bids; `A[...]` means asks; `id@price:quantity` describes an order.
Empty sides are omitted; `empty` means both sides are empty.

Event notation: `Accepted(id)`, `Rested(id,remaining)`, `Filled(id)`,
`Partial(id,remaining)`, `Expired(id,remaining)`, and `Cancelled(id,remaining)`
refer to the corresponding full event names above. `Trade(aggressive,resting,
price,quantity)` means one `Execution`. `Rejected(reason)` and
`CancelRejected(reason)` include the incoming request's IDs. Arrows show exact
event order; payload fields omitted here are determined by sections 1-10.

| Scenario | Starting book | Request | Ordered events | Final book |
|---|---|---|---|---|
| Passive bid | empty | I: buy limit 100, qty 5 | Accepted(I) -> Rested(I,5) | B[I@100:5] |
| Passive ask | empty | I: sell limit 101, qty 5 | Accepted(I) -> Rested(I,5) | A[I@101:5] |
| Non-crossing sides | B[b1@100:5] | I: sell limit 101, qty 3 | Accepted(I) -> Rested(I,3) | B[b1@100:5], A[I@101:3] |
| Full crossing at equality | A[a1@101:5] | I: buy limit 101, qty 5 | Accepted(I) -> Trade(I,a1,101,5) -> Filled(a1) -> Filled(I) | empty |
| Partial resting fill | A[a1@101:5] | I: buy limit 102, qty 2 | Accepted(I) -> Trade(I,a1,101,2) -> Partial(a1,3) -> Filled(I) | A[a1@101:3] |
| Aggressive remainder rests | A[a1@101:2] | I: buy limit 102, qty 5 | Accepted(I) -> Trade(I,a1,101,2) -> Filled(a1) -> Partial(I,3) -> Rested(I,3) | B[I@102:3] |
| Three same-price FIFO orders | A[a1@101:2,a2@101:3,a3@101:4] | I: buy limit 101, qty 7 | Accepted(I) -> Trade(I,a1,101,2) -> Filled(a1) -> Partial(I,5) -> Trade(I,a2,101,3) -> Filled(a2) -> Partial(I,2) -> Trade(I,a3,101,2) -> Partial(a3,2) -> Filled(I) | A[a3@101:2] |
| Walk ask levels | A[a1@101:2,a2@102:3,a3@103:4] | I: buy limit 102, qty 6 | Accepted(I) -> Trade(I,a1,101,2) -> Filled(a1) -> Partial(I,4) -> Trade(I,a2,102,3) -> Filled(a2) -> Partial(I,1) -> Rested(I,1) | B[I@102:1], A[a3@103:4] |
| Walk bid levels | B[b1@100:2,b2@99:3] | I: sell limit 99, qty 4 | Accepted(I) -> Trade(I,b1,100,2) -> Filled(b1) -> Partial(I,2) -> Trade(I,b2,99,2) -> Partial(b2,1) -> Filled(I) | B[b2@99:1] |
| Cancel first | B[b1@100:2,b2@100:3,b3@100:4] | cancel b1 | Cancelled(b1,2) | B[b2@100:3,b3@100:4] |
| Cancel middle | B[b1@100:2,b2@100:3,b3@100:4] | cancel b2 | Cancelled(b2,3) | B[b1@100:2,b3@100:4] |
| Cancel final | B[b1@100:2,b2@100:3,b3@100:4] | cancel b3 | Cancelled(b3,4) | B[b1@100:2,b2@100:3] |
| Cancel only order | A[a1@101:2] | cancel a1 | Cancelled(a1,2) | empty |
| Cancel partially filled order | A[a1@101:3], originally qty 5 | cancel a1 | Cancelled(a1,3) | empty; prior executions unchanged |
| Cancel unknown or inactive | B[b1@100:2] | cancel x, a nonzero inactive ID | CancelRejected(NotActive) | B[b1@100:2] |
| Cancel another owner's order | B[b1@100:2] | participant 2 cancels b1 | CancelRejected(NotOwner) | B[b1@100:2] |
| Market with enough liquidity | A[a1@101:5] | I: buy market, qty 5 | Accepted(I) -> Trade(I,a1,101,5) -> Filled(a1) -> Filled(I) | empty |
| Market with insufficient liquidity | A[a1@101:2] | I: buy market, qty 5 | Accepted(I) -> Trade(I,a1,101,2) -> Filled(a1) -> Partial(I,3) -> Expired(I,3) | empty |
| Market against empty book | empty | I: sell market, qty 3 | Accepted(I) -> Expired(I,3) | empty |
| Invalid price | B[b1@100:2] | I: buy limit 0, qty 1 | Rejected(InvalidPrice) | B[b1@100:2] |
| Invalid quantity | B[b1@100:2] | I: buy limit 100, qty 0 | Rejected(InvalidQuantity) | B[b1@100:2] |
| Duplicate active ID | B[b1@100:2] | ID b1: sell limit 100, qty 1 | Rejected(DuplicateOrderId) | B[b1@100:2] |
| Duplicate terminal ID | empty; b1 previously accepted and cancelled | ID b1: buy limit 100, qty 1 | Rejected(DuplicateOrderId) | empty |

All 23 rows are executable in `tests/exchange_scenario_tests.cpp`, registered as
`exchange_scenario` with CTest. Each scenario creates a fresh exchange, prepares
its starting state through raw requests, and compares the entire ordered event
stream and final book to explicit expectations. Checks include full payloads and
metadata, best/FIFO heads, all expected active records with original acceptance
priority, final time/counters, and unchanged previously returned event buffers.

The existing request/book/exchange tests add both-side and boundary coverage for
invalid enums, zero/unknown IDs, price/quantity maxima, partial-fill priority,
equal timestamps, validation precedence, and counter capacity. The canonical suite
also checks book structural invariants after each preparation step and the final
request. `tests/order_book_invariant_tests.cpp` adds primitive-operation and
corruption-detection coverage, while allocation tests validate failure/retry state.
`tests/exchange_property_tests.cpp` supplies controlled generated workloads and
execution reconciliation. An independent flat scan of a test-owned ledger checks
best price/FIFO against each returned execution. Both order remainders are reduced
by the checked minimum quantity; reported status, resting, expiry and cancellation
values must agree with that ledger. Active records, original acceptance metadata,
clock/counters and structural invariants are checked after every request.
Generation uses a fixed seed, nonmonotonic IDs, three participants, bounded active
state and occasional maximum quantities. Failure reports the seed, request number
and complete input prefix; a test-only execution perturbation verifies exact replay.
The default CTest run repeats four 5,000-request workloads. Larger bounded-state
runs are available through the seed/count arguments documented in the README.

## Specification review

- [x] Every supported rule above has a defined outcome.
- [x] Every canonical example has an unambiguous final state and event order.
- [x] No rule depends on wall-clock timing or thread scheduling.
- [x] No implementation data structure has been chosen without a requirement.
