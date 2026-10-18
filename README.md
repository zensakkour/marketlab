# MarketLab

> Deterministic local exchange infrastructure for quantitative trading research and trading-system testing.

MarketLab is designed to provide a controlled electronic-market environment in which strategies and trading systems can interact with exchange state under reproducible conditions. It addresses experiments that ordinary historical backtests cannot model faithfully, including order-level interaction, queue priority, liquidity, latency, competing participants, and the sequence of events that produces an execution.

Unlike a conventional backtester, which generally starts from prices and trades that have already occurred, MarketLab targets the process that creates them:

**strategy decision -> order submission -> exchange processing -> matching -> execution -> market update -> strategy reaction**

## Initial scope

The initial scope focuses on a deterministic local exchange simulation with:

- continuous limit order books and price-time priority;
- limit and market orders, cancellations, partial fills, and executions;
- exact internal prices and deterministic logical time;
- controlled synthetic participants and reproducible scenarios;
- participant positions, cash, and profit-and-loss accounting;
- multiple independent instruments and simulated latency;
- event recording and a research-facing interface after the core is stable.

The first reference experiment targets a market-making strategy trading against controlled background participants. Live trading, brokerage connectivity, production exchange protocols, derivatives, multi-venue routing, and a sophisticated GUI are outside the initial scope.

## Engineering principles

- Correct exchange semantics come before performance.
- Identical configurations, engine versions, starting states, and seeds should produce identical logical outcomes.
- Prices inside the matching core use exact representations.
- Each order book has one logical writer; independent instruments may later have separate owners.
- Simulated timing uses logical time rather than wall-clock delays.
- Positions and cash change through executions, and market data is derived from exchange state.
- Performance work follows correctness, testing, and reproducible measurement.

## Development workflow

Development is organized around small, validated implementation steps. After each completed step, the local `PROGRESS.md` is updated with completed checklist items, concise implementation details, relevant validation, important design decisions, and real benchmark results where applicable. The implementation sequence prioritizes specification -> correctness -> testing -> measurement -> optimization. `PROGRESS.md` remains local and is not part of the public repository.

Phase 8 accounting now covers execution history, bought/sold volumes, exact net
positions, cash flows and FIFO realised/unrealised/total PnL at caller-supplied
prices in a separate participant ledger. Phase 9 now includes owning scripted
participants, controlled noise, refreshed liquidity and market-order taker plans,
with a seeded standalone example. Phase 10 adds a four-participant reference
market-making experiment with exact duration-weighted spread and full accounting
reports. See
[participant accounting](docs/participant-accounting.md) and
[controlled participants](docs/controlled-participants.md) for the current scope.
The [reference experiment](docs/reference-market-making.md) is a reproducible
demo and regression fixture. Phase 11 adds an immutable
[instrument registry](docs/instrument-configuration.md) and
[single-writer multi-instrument routing](docs/multi-instrument-exchange.md),
with independent books, clocks, counters, market data and accounting.
Phase 12 begins with a validated, immutable
[instrument-to-worker assignment plan](docs/worker-ownership.md) and a
[caller-driven partitioned simulation](docs/partitioned-simulation.md), with
private queues, routed processing and owned step results.
[Threaded closed batches](docs/threaded-simulation.md) now process independent
partitions concurrently with joined, deterministic publication. Closed-batch
concurrency tests also pass under Linux ThreadSanitizer. Live ingress and
backpressure remain open.

## Build, run, and test

The current C++23 components are exact integer-tick prices, nonnegative
quantities with checked subtraction, distinct order/participant/instrument IDs,
order sides/types, logical timestamps, instrument tick configuration,
order/cancel requests, accepted/resting orders, executions, exchange events,
exact text event recording, scheduled request records, an owning arrival queue
and simulation delivery, owned market snapshots with exact spread/midpoint,
last trade, cumulative volume and top-N depth,
passive order-book storage, single-match limit fills,
read-only limit match counting, complete accepted-order processing for limits and
markets, active-order lookup, owned-order cancellation, and raw order/cancel
processing through a single-instrument exchange, plus downstream participant
quantity/cash-flow accounting with failure-atomic execution batches and history replay.
Logical timestamps count unsigned 64-bit nanoseconds from the simulation origin.
Tick sizes use a positive decimal coefficient and a scale from 0 through 18;
instrument configuration requires a nonzero ID and a nonempty symbol.

`InstrumentRegistry` owns a fixed copied instrument list. It rejects invalid
records and duplicate IDs, provides non-allocating lookup with `find(id)` and
a read-only `instruments()` span in increasing ID order. Borrowed records remain
valid until destruction. The registry contains immutable configuration.
See [instrument configuration](docs/instrument-configuration.md).

`MultiInstrumentExchange(registry, participants)` owns one existing `Exchange`
per instrument and routes raw orders/cancels by ID. Instrument clocks, sequences
and lifetime client IDs remain local; unknown destinations return
`UnknownInstrument` without an event or mutation. `find(id)` borrows a const
exchange for existing market-data projections. `Exchange::instrument()` borrows
its immutable symbol/tick configuration without allocation. Caller-owned ledgers
remain separate per instrument; the existing `Simulation` remains mono-instrument.
See [multi-instrument routing](docs/multi-instrument-exchange.md).

`WorkerAssignmentPlan(registry, worker_count, assignments)` copies one explicit
`InstrumentWorkerAssignment{instrument_id, worker_index}` per instrument. It
requires a positive worker count and indexes in `[0, worker_count)`; missing,
unknown and duplicate assignments are refused. Idle workers are allowed.
`worker_for(id)` returns an optional index, and `assignments()` borrows a sorted
const span; reads allocate nothing. This is fixed configuration for future worker
ownership. See [worker assignment](docs/worker-ownership.md).

`PartitionedSimulation(registry, plan, participants)` owns one exchange group
and arrival queue per active worker. `schedule(request)` selects its instrument
owner; `step(worker_index)` commits that worker's earliest arrival and returns
its owned events. Scheduling checks the target instrument's committed time;
engine/allocation failure retains the pending arrival for retry. Arrival sequences
are worker-local. All calls are serialized, and returned batches follow the
caller's step order across workers. See [partitioned simulation](docs/partitioned-simulation.md).

`ThreadedSimulation(registry, plan, participants)` accepts scheduled inputs between
`run()` calls. Each run uses one exclusive thread per nonempty partition, joins
all writers, and returns owned `WorkerBatch` results sorted by worker index.
Worker failures retain their committed prefixes and pending heads; failed startup
processes nothing. Public calls and borrowed-view reads must stay outside the
active run. See [threaded batches and recovery](docs/threaded-simulation.md).

For v0, a participant is represented by its `ParticipantId`. Orders and executions
carry that identity directly. Participant registration belongs to fixed
`Exchange` configuration. `ParticipantLedger` owns downstream execution history,
gross volumes, net positions and cash flows. FIFO projections derive realised,
unrealised and total PnL from history. Controlled request producers prepare
scripts outside the exchange; reactive strategies remain later work.

Submitted order quantities must be positive; zero is supported for remaining
quantities after a complete fill. Requests retain raw quantity and tick-price
values so invalid submissions can be rejected explicitly. `validate_fields()`
checks numeric IDs, side/type, quantity, and price fields in the specified order
and returns an optional rejection reason. `Exchange::process_order()` checks
registration, lifetime duplicates, and clock ordering; passing field validation alone does not accept an order.

Accepted orders retain their original quantity and request sequence for arrival
priority. Resting records require a limit order and a positive remainder no
greater than its original quantity. Executions carry both order IDs, buyer and
seller IDs, the aggressive side, exact price and quantity, logical time, and
distinct request/event sequences. Record `is_valid()` checks intrinsic consistency;
the exchange establishes acceptance and delegates book membership and matching
to its owned book. One execution represents one trade, including permitted
self-matches.

`ExchangeEvent` is a `std::variant` of the nine contract events: acceptance,
execution, partial fill, full fill, resting, market-remainder expiry, cancellation,
order rejection, and cancel rejection. Successful records require valid numeric
IDs and quantities consistent with their status. Rejections retain the submitted
request, including invalid fields, and its rejection reason. Backward-time
rejections preserve the submitted timestamp while carrying current logical time.
`OrderBook::process_order()` produces accepted limit/market events, and its
`cancel()` returns a successful cancellation record. `Exchange::process_order()`
accepts raw new orders and produces their accepted events or one `OrderRejected`.
`Exchange::process_cancel()` returns one `OrderCancelled` or `CancelRejected`.

`SimulationEvent` holds a raw order or cancel request and a distinct simulation
sequence. The request timestamp is its scheduled exchange-arrival time.
`scheduled_before()` orders records by logical time, then scheduling insertion
sequence; order IDs and request kind do not break ties. Scheduling records may
contain malformed requests for the exchange to reject.

`SimulationQueue` owns pending arrivals in a vector-backed heap. Its
`schedule(SimulationRequest)` copies the raw payload and returns the assigned
`SimulationSequence` in a `std::expected`. Successful insertion assigns the next
positive sequence; failed insertion consumes none. `SequenceExhausted` and
`CapacityExceeded` are storage diagnostics, not exchange rejections. Allocation
exceptions propagate and preserve all pending requests, their ordering, the
counter and existing borrowed views.

`next()` returns a read-only borrowed pointer to the earliest arrival, or
`nullptr`; reacquire it after a successful schedule/pop operation.
`pop_next()` removes and returns that arrival without allocating, or returns
`std::nullopt` for an empty queue. `empty()`, `size()` and `last_sequence()`
report queue state. Draining the queue retains its sequence counter for later
insertions. The queue cannot be copied or moved. Its timestamps are ordering keys;
clock advancement and request delivery belong to its owning simulation.

`Simulation` owns one exchange and its arrival queue. Construction starts at t=0.
`schedule()` refuses timestamps before `current_time()` with
`SimulationScheduleError::InvalidTimestamp`, without assigning a sequence or
producing an exchange event. Equal-time and future arrivals are allowed; other
raw fields remain available for exchange validation. Queue sequence/capacity
errors map to `SimulationScheduleError`; allocation exceptions preserve state.

`step()` processes the earliest arrival through the existing raw exchange entry
point. It returns `std::expected<std::optional<SimulationStep>, ExchangeError>`:
an empty optional means the queue is drained; a step owns the original scheduled
arrival and its exchange-event vector; an unexpected value means an engine error.
Order/cancel rejections are committed processing and consume their arrival.
On an engine error or allocation exception, the pending arrival and committed
clock/book/counters remain unchanged. Result transfer after commit cannot allocate.

The exchange clock is the single source of current logical time. Empty steps do
not advance it. A drained simulation can resume when new current/future requests
are scheduled; its identities and counters continue. `exchange()` and `queue()`
expose only const views, preserving the single writer. Drive it by calling `step()`
until empty, stopping on an engine error. No wall-clock sleeps, callbacks or
automatic participant accounting are introduced inside the runner. A separate
ledger consumes returned executions. Delayed reports and latency remain later
components.

`best_prices(exchange)` from `<marketlab/market_data.hpp>` returns an owned
`BestPrices` snapshot: configured instrument, committed time/request/event
counters, and optional exact bid/ask prices. Reads allocate nothing and emit no
events. The copied prices remain valid after later fills/cancels; call again for
current state. Query only at completed request boundaries under the single writer.
Pending simulation arrivals are excluded. `spread_ticks()` returns ask minus bid;
`mid_price()` returns exact `MidPrice{whole_ticks, half_tick}`. Both are absent
when a side is missing, and neither allocates or rounds. For bid 100 / ask 101,
spread is 1 and mid is `{100, true}` (100.5 ticks). Arithmetic stays exact near
INT64_MAX. See [market data](docs/market-data.md) for fields and current scope.

`market_snapshot(exchange)` combines those prices with an owned optional last
`Execution` and exact cumulative executed volume. `Exchange::last_trade()` also
copies the last execution directly.
The final execution sequence determines the last trade, including equal-time
fills. Non-trading requests retain it; failures preserve it. Its trade timestamp
may precede current snapshot time. Both reads are non-allocating and `noexcept`.

`depth_snapshot(exchange, N)` adds up to N best price levels per side, with total
remaining units and active order counts. Bids descend and asks ascend; N=0 returns
no levels. Reads own their vectors and may allocate; allocation failure leaves
exchange state unchanged. `OrderBook::depth(N)` exposes the underlying projection.

Level totals and cumulative volume use `AggregateQuantity`, two unsigned 64-bit
words with checked addition/subtraction and exact decimal `marketlab::to_string()`.
Totals remain exact above UINT64_MAX; v0 request/order bounds keep them below
2^127.
Volume counts each execution once, including self-matches, and excludes all
unfilled remainders. No quantity is rounded or saturated. See
[market data](docs/market-data.md) for arithmetic bounds and ownership.

`TimeWeightedSpread` from `<marketlab/time_weighted_spread.hpp>` observes
explicit timestamp/spread samples without allocation or exchange mutation.
`observe()` integrates the previous sample; `snapshot(horizon)` projects the
last state without advancing the observer or exchange. It separates missing-price
duration and retains the exact two-word tick-nanosecond integral and mean fraction.
See [spread semantics and bounds](docs/reference-market-making.md#exact-time-weighted-spread).

`ParticipantLedger` from `<marketlab/participant_ledger.hpp>` owns quantity
accounts for one instrument and fixed participants, initially zero. `apply()`
accepts a span of committed executions as one complete batch. It rejects invalid,
foreign, unknown-counterparty or out-of-order records without mutation, and
allocation failure leaves accounts/history unchanged for retry. `account(id)`
returns an owned optional with `buy_volume`, `sell_volume` and exact sign/magnitude
`position()`. `history()` borrows the accepted executions; reacquire after ingestion.
Rebuild into a fresh ledger from that history. Self-matches increase both gross
volumes and leave net position unchanged. Accounts also expose `buy_notional`,
`sell_notional` and derived `cash_flow()` as an exact sign/magnitude value.
Purchases debit cash, sales credit it, and self-matches leave net cash unchanged.
Flows start at zero, in units of price ticks times quantity; negative cash is
allowed. `CashAmount` stores checked 192-bit amounts, preserving valid totals
beyond 128 bits. Monetary interpretation requires the configured instrument's
`TickSize`. Checked `multiply(uint64_t)` scales a `CashAmount` by an integer,
returning an empty optional if the product exceeds 192 bits.
`from_aggregate(Price, AggregateQuantity)` computes an exact price-times-quantity
amount without narrowing cumulative quantities to 64 bits. It selects no market
price or PnL policy.

`ParticipantAccount::total_pnl(Price mark)` derives exact signed total PnL as net
cash plus position times an explicit whole-tick price. It allocates nothing and
changes no account state. The optional result is always present for ledger
accounts; arbitrary caller-created values return an empty optional on overflow.
`fifo_pnl(ledger, participant, mark)` from `<marketlab/participant_pnl.hpp>` adds
FIFO realised/unrealised decomposition as an owned `PnlBreakdown`. It reconstructs
temporary lots on demand; allocation may throw without changing ledger state.
`PnlAmount` is the shared exact sign/magnitude type; `TotalPnl` remains an alias.
Funding, solvency checks and fees remain outside these APIs.

`SimulationRandom(seed)` supplies reproducible inclusive integer samples via
`uniform(minimum, maximum)`, using rejection sampling without floating point.
Invalid/singleton ranges consume no random word; a full uint64 range is supported.
`OrderIdSource(first)` supplies consecutive client order IDs through `next()` and
non-consuming `peek()`. Zero is invalid; UINT64_MAX is usable once, then the source
reports exhaustion. The experiment owns both sources. Copies are checkpoints,
not disjoint streams or ID namespaces; use one coordinated ID source per exchange.
Neither source changes exchange sequences or bypasses duplicate validation.

`ScriptedParticipant(id, span)` owns one participant's ordered request list and
schedules one action at a time, advancing only on successful insertion.
`make_noise_script()`, `make_liquidity_script()` and `make_taker_script()`
prepare bounded plans around explicit configuration. Noise uses uniform integer
gaps, quantities and price distances with buy/cancel probabilities; the provider
cancels its previous quotes before refreshing fixed-reference bid/ask pairs;
the taker submits markets. Plan failures preserve caller RNG/ID state for retry.
Scripts preserve raw exchange validation and equal-time insertion priority.
See [controlled participants](docs/controlled-participants.md) for all fields,
random draw order, ownership, cancellation behavior and failure guarantees.

The matching contract is in [docs/matching-semantics.md](docs/matching-semantics.md).
`OrderBook` owns passive resting limits for one nonzero instrument ID. It stores
ordered price levels with FIFO queues and tracks active IDs. `insert_passive()`
checks record validity, instrument, duplicate IDs, increasing arrival sequences,
and a non-crossing price. Failures leave the book unchanged, including allocation
failures. `best_bid()` and `best_ask()` return read-only pointers to the FIFO head
at the best price, or `nullptr` for an empty side; reacquire pointers after mutation.

`find(OrderId)` returns the current active resting record by ID, including orders
behind a FIFO head and at other price levels, or `nullptr` for zero/absent IDs.
It returns a read-only borrowed pointer; reacquire it after mutation. Lookup checks
the active-ID set, then scans owned queues without allocation or state changes.
Present-order lookup is linear in active orders; no performance claim is made.
It provides current ownership and remainder data for cancellation.
The book cannot be copied or moved. Its insertion results are storage diagnostics,
not exchange rejections: `WouldCross` means matching is required.

`check_invariants()` is a const diagnostic for completed book operations. It checks
non-crossing best prices, nonempty levels, valid positive bounded resting
quantities, instrument/side/price alignment, strict FIFO arrival priority, unique
active IDs and request sequences, and exact agreement with the active-ID index.
It uses temporary sets (O(N log N) work and O(N) memory for N resting orders);
allocation failures propagate without changing the book. It is called explicitly
by validation code after processing, when resting state is complete.

`match_one()` matches an accepted limit remainder against at most one best-priced
opposite FIFO head. It returns `std::expected<std::optional<SingleMatch>, MatchError>`:
an error means invalid operation input, an empty optional means no eligible order,
and a `SingleMatch` contains the execution and both remaining quantities. Execution
price is the resting price; quantity is the smaller remaining quantity. Partial
resting fills keep priority; full fills remove the order, its active ID, and any
empty price level. The incoming record is unchanged and its remainder is returned
to the caller. Matching allocates no memory and permits the specified self-matches.

`match_count()` returns the number of executions needed for an accepted limit
remainder against the current book, or a `MatchError` for invalid input. It walks
eligible price levels in FIFO order without changing the book or allocating memory,
and stops when the remainder is filled or the limit excludes further liquidity.
It uses current remaining quantities and avoids summing liquidity, which could
overflow. This supports capacity checks before complete limit processing;
the single writer must keep the book unchanged between counting and matching.
The count does not reserve output storage, assign sequences, or process a request.

`process_order()` handles a complete accepted limit or market order across opposite
price levels. Limits stop at their price boundary and rest any positive remainder
at their original limit and arrival priority. Markets have no price boundary,
never rest, and emit `MarketRemainderExpired` for any unfilled remainder, including
the entire quantity when opposite liquidity is empty. Its
`OrderResult` contains the ordered `ExchangeEvent` records and remaining quantity:
acceptance, then execution/resting status/incoming status for each match, followed
by resting or market-remainder expiry when applicable. The caller supplies the
first event sequence; the operation checks the exact contiguous range without
wrapping before changing state.
It reserves event storage and prepares any resting remainder before consuming
liquidity. Diagnostic errors or allocation failures leave the book unchanged;
allocation failures propagate. Successful processing advances book arrival priority,
even when the incoming order is fully filled.

`cancel(request, request_sequence, event_sequence)` removes an active order owned
by the requesting participant and returns its `OrderCancelled` record. It checks
the book instrument, nonzero participant/target IDs, increasing positive request
sequence, positive event sequence, active status, and ownership before mutation.
The record contains the cancellation timestamp, supplied sequences, owner, price,
and remaining quantity. Removal preserves survivor FIFO/metadata and deletes empty
levels and active IDs. The operation allocates no memory. `CancelError` diagnostics
leave the book unchanged; successful cancellation advances book arrival priority.
`Exchange::process_cancel()` establishes clock/registration validity, assigns
sequences, and creates `CancelRejected` events for raw cancellation failures.

Book processing consumes trusted accepted records. `Exchange` owns one configured
instrument, a fixed set of registered participants, its book, lifetime accepted
IDs, logical time, and request/event counters. Construct it with an `Instrument`
and a `std::span<const ParticipantId>`; configuration is copied, zero participant
IDs are invalid, repeated registrations name the same participant, and an empty
participant set accepts no orders. The instrument must be intrinsically valid.

`Exchange::process_order(OrderRequest)` returns
`std::expected<std::vector<ExchangeEvent>, ExchangeError>`. A value contains the
complete accepted-order stream or exactly one rejection preserving the raw request.
Validation follows the contract's timestamp, registration, field, then lifetime-ID
precedence. Filled and expired accepted IDs remain reserved. Rejected IDs are reusable.
Non-backward requests advance logical time even when rejected; backward requests
are rejected at current time. Requests and events are numbered from one, including
rejections; equal timestamps retain call order.

`Exchange::process_cancel(CancelRequest)` returns the same expected event-vector
type, with exactly one returned event. It validates timestamp, configured
instrument/participant, and target ID before active status and ownership. Valid
owned cancellation removes only the remaining quantity; inactive targets produce
`NotActive`, and active targets owned by another participant produce `NotOwner`.
Cancelled IDs stay reserved for the exchange lifetime. Both entry points share
logical time and counters, including rejections. Cancellation prepares its event
storage before removing the order, so an allocation failure changes no state.

`ExchangeError` reports request/event sequence exhaustion or unrepresentable output
capacity, without processing the request; stop the run on a capacity error.
Allocation exceptions propagate and leave book, ID history, time, and counters
unchanged, permitting an identical retry. All potentially allocating work precedes
fills and state commitment. `book()` exposes only a borrowed const view; reacquire
resting-order pointers after processing. `current_time()`, `last_request_sequence()`,
and `last_event_sequence()` expose committed state (both sequences start at zero
before the first request). One writer processes calls without callbacks.
Reactive strategy behavior, modeled latency and the decimal price adapter
remain future work.

### Requirements

- A C++23 compiler; the current code has been validated with GCC 15.2 on Windows.
- CMake 3.20 or newer.
- Ninja.

The compiler, `cmake`, `ctest`, and `ninja` must be available on your terminal's
`PATH`. Open a terminal in the repository root, which contains `CMakeLists.txt`.
The commands below work in PowerShell and ordinary Unix shells.

### Debug build and tests

Configure the build directory, compile all targets, then run all registered tests:

```powershell
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Run each command after the previous one succeeds. Configuration creates the
build files; building compiles the test executables; CTest runs them. Tests are
enabled explicitly, and compiler warnings are treated as errors.

A successful test run ends with `100% tests passed, 0 tests failed`.
Currently there are forty-six registered tests: `price`, `aggregate_quantity`,
`unsigned_product`, `cash_amount`,
`domain`, `instrument_registry`, `worker_assignment_plan`, `multi_instrument`,
`multi_instrument_allocation`, `multi_instrument_workload`,
`partitioned_simulation`, `partitioned_simulation_allocation`, `partitioned_market`,
`threaded_simulation`, `threaded_simulation_allocation`,
`requests`, `orders`,
`events`, `event_record`, `market_data`, `time_weighted_spread`, `participant_ledger`,
`participant_ledger_allocation`, `participant_pnl`, `participant_fills`, `order_id_source`,
`simulation_random`, `scripted_participant`, `participant_plans`,
`participant_allocation`, `participant_integration`, `market_making`,
`simulation`, `simulation_queue`,
`simulation_queue_allocation`,
`simulation_delivery`, `order_book`, `order_book_allocation`, `order_book_matching`,
`order_book_limit`, `order_book_cancel`, `order_book_market`, `order_book_invariant`,
`exchange`, `exchange_scenario`, and `exchange_property`.
`--output-on-failure` prints diagnostics from failing tests, and CTest returns a
nonzero exit code on failure.

`instrument_registry` checks copied ownership, sorted lookup/views, boundary
IDs/tick sizes, duplicate/invalid definitions, constructor allocation failure
and retry, and allocation-free reads.

`worker_assignment_plan` checks complete mappings, invalid bindings, idle workers,
input-order independence, owned storage, boundary indexes, construction failure
and retry, and non-allocating queries.

`multi_instrument` compares every routed request against isolated exchanges,
checks instrument-local time/IDs/counters, unknown routing and ledger isolation,
and repeats the literal two-instrument CLI result and event checksum twenty times.
`multi_instrument_allocation` injects every construction/processing allocation
failure, verifies unchanged target/foreign state and retries identically; lookup
and unknown routing also run with allocation disabled.

`multi_instrument_workload` compares 18,000 generated requests across 1, 8 and
32 symbols against isolated exchanges. Its random draws have an explicit order,
with a fixed-seed regression prefix independent of argument evaluation order.
`partitioned_simulation` repeats 28,800
requests with 1/4/7 partitions and different worker step orders, comparing each
instrument's event stream, book, market data, cash, positions and FIFO PnL.
`partitioned_simulation_allocation` verifies failed construction, scheduling and
processing retain their inputs/pending arrivals and retry identically.
`partitioned_market` locks the three-instrument example's literal reports and
event checksums over repeated runs with 1 through 4 workers in both caller-driven
and actual-thread modes.

`threaded_simulation` compares two consecutive batches against sequential
partitions and replays the reference experiment on actual threads. A further
16,640 arrivals exercise large equal-time backlogs, uneven partition loads, idle
workers, FIFO fills and result ownership across consecutive runs.
`threaded_simulation_allocation` injects coordinator and worker failures to check
the startup gate, preserved committed prefixes, pending-head retry and independent
worker completion. The dedicated Linux ThreadSanitizer command below runs these
two suites and the actual-thread CLI regression with race instrumentation.

`aggregate_quantity` checks exact 128-bit decimal boundaries, checked overflow,
constexpr operations and 10,000 generated additions against independent decimal
arithmetic. `market_data` verifies depth, volume, best prices and metadata after
book transitions, retained snapshots, last executions, exact spread/midpoint
arithmetic, numerical boundaries,
rejections and scheduled delivery. Generated workloads compare snapshots against
the independent execution ledger. Allocation tests verify non-allocating reads
and depth allocation failures followed by retry.

`unsigned_product` checks the portable 64-by-64 multiplication primitive with
4,096 single-bit pairs and 10,000 independent decimal products. `cash_amount`
checks 192-bit arithmetic and decimal boundaries, overflow/borrow/carry, and
10,000 generated trade products and accumulated sums, plus 10,000 scalar products
checked against independent decimal arithmetic, including overflow. Another
10,000 cases verify full-width aggregate quantities times exact prices.

`participant_ledger` checks initial state, account snapshots, signed positions
and cash flows, total PnL at explicit prices, self-matches,
invalid/order/registration refusal, whole-batch rollback, correction,
full and incremental replay, quantity totals beyond 64 bits and cash beyond
128 bits. `participant_ledger_allocation` injects each map/history allocation failure, preserves prior history views and
verifies one-time retry from retained, already committed simulation executions.
Generated workloads also compare cash, positions and total PnL at three marks
to independent decimal arithmetic
and replay their full execution history.
`simulation_random` checks seed replay, full-range engine agreement, exact
power-of-two mapping, rejection-sequence goldens and bounded sampling.
`order_id_source` checks zero rejection, configurable starts, non-consuming reads,
checkpoints and final-ID exhaustion without wrap.

`scripted_participant` checks ownership, copied requests, equal-time order, raw
validation and scheduling retry. `participant_plans` checks plan policies,
shared IDs, seeded replay, bounds and failure rollback. `participant_allocation`
injects each plan allocation failure and checks script construction/scheduling
retry. `participant_integration` repeats four seeds, compares exact event bytes
and the default summary, and independently reconstructs accounts and final depth.

`participant_pnl` checks FIFO lot selection, long/short closes and reversals,
self-matches, owned results, full-width amounts and 1,000 generated executions
against an independent unit-lot/decimal oracle. Allocation tests exercise every
FIFO scratch-allocation failure, unchanged history and query retry.

`participant_fills` checks direction/liquidity-role counts, self-matches,
partial and multi-level fills, unchanged results after refused ingestion, owned
values, replay and size-independent counting. The existing allocation test checks
non-allocating reads; integrated seed replay independently reconciles fill roles.

`time_weighted_spread` checks interval weighting, missing/zero/tied samples,
invalid observations, read-only projections, allocation-free paths, maximum time
and price ranges and 10,000 independent decimal interval calculations.
`market_making` verifies the literal summary and event checksum, four repeated
seeds, exclusive horizons, participant priority, quote refresh and drained-queue
observation. Independent active-order, unit-lot FIFO and unit-time spread
oracles reconcile the final report and direct exchange event replay.

`event_record` checks all nine exact output lines, raw rejection fields, integer
limits, locale/stream-setting independence and output errors. Recording the
scripted timeline leaves its event sequence and final book identical to the
unrecorded run.

`simulation_queue` checks time/insertion ordering, zero/maximum timestamps,
sequence exhaustion arithmetic, insertion after removal, mixed request kinds and
complete malformed payload preservation across heap growth. Allocation tests
inject each insertion allocation failure with 0 through 65 pending arrivals,
verify unchanged state and retry priority, and remove arrivals with allocations
disabled.

`simulation_delivery` uses the runner with six requests scheduled in a different
order from their arrival times. It exercises equal-time FIFO, passive bids/ask,
a partial execution and owned cancellations. It checks all twelve exact
ordered event records, intermediate book heads/invariants, clock/counters and the
surviving order's original metadata. Twenty fresh replays must reproduce the same
complete event/execution sequence and final book. Lifecycle checks cover start,
empty steps, committed rejections, past-time refusal, equal-time resumption and
maximum logical time.
The allocation target also checks retained arrivals, unchanged exchange state
and retry after failure during limit/market matching, cancellation and rejection.

`exchange_scenario` runs all 23 canonical rows from the matching contract through
the raw exchange entry points. It checks complete ordered event records, final
active orders and FIFO heads, original order metadata, time/counters, and preserved
previously returned events. Structural invariants are checked after each prepared
request and the final request. `order_book_invariant` exercises the public book
operations and deliberately corrupts/restores test-owned records to check detection.
Allocation tests check invariants after failure/retry and verify the diagnostic
itself preserves state when its temporary allocations fail.

`exchange_property` runs four fixed seeds, 5,000 requests per seed, twice
(40,000 verified requests). A separate execution ledger checks prices, price/FIFO
priority, counterparties, bounded quantity subtraction, status/remainder events,
active records, lifetime duplicates, clock/counters and structural invariants
after each request. Test generation uses one seeded `std::mt19937_64` with explicit
integer mapping, nonmonotonic client IDs, three registered participants, and up to
64 active orders. It includes passive/aggressive limits, markets, successful and
rejected cancels, invalid new orders and maximum signed quantities. A deliberate
error in a returned execution is detected and reproduced from its seed and request
number; the production matching path is unchanged.

After editing C++ code, rerun the build and CTest commands. Rerun configuration
after changing `CMakeLists.txt` or build options.

GCC 15.2 documents `-fanalyzer` as suitable only for C; it reports spurious
diagnostics in the C++ standard containers used here. See the
[GCC analyzer documentation](https://gcc.gnu.org/onlinedocs/gcc-15.2.0/gcc/Static-Analyzer-Options.html).
If an older local build cached that flag, reset its extra compiler flags once:

```powershell
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS=""
```

### Run the controlled market

`controlled_market` is a standalone executable, available with tests enabled
or disabled. It runs a fixed-reference liquidity provider, noise flow and a
market-order taker through the normal simulation and execution-accounting path.

In PowerShell, after the Debug build:

```powershell
.\build\debug\controlled_market.exe
$LASTEXITCODE
.\build\debug\controlled_market.exe 123
$LASTEXITCODE
.\build\debug\controlled_market.exe --events 42 | Set-Content -Encoding ascii -ErrorAction Stop .\build\debug\controlled-events.log
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/controlled_market
./build/debug/controlled_market 123
./build/debug/controlled_market --events 42 > build/debug/controlled-events.log
```

The default seed is 42. Custom seeds accept decimal digits from 0 through
UINT64_MAX. The default output has five lines: aggregate counts, three
participant accounts with FIFO PnL, and final depth. Its first line is:

```text
seed=42 instrument_id=1 mark_ticks=100 requests=120 events=317 orders=93 cancellations=27 executions=45 time_ns=350 volume_units=91 bid_ticks=99 ask_ticks=101
```

At the explicit mark of 100 ticks, final positions are +8, +5 and -13,
cash flows are -720, -445 and +1165, and total PnLs are +80, +55 and -135.
Amounts use exact tick-times-quantity units. Order/cancel counts include rejected
attempts. `--events` prints the 317 existing v1 event records for seed 42 instead
of the summary. The seed and ordered producer calls determine reproducibility.
See [configuration and scheduling order](docs/controlled-participants.md#integrated-executable-and-regression).

Exit codes are 0 for success, 1 for run/output failure and 2 for invalid
arguments. Negative, fractional, out-of-range or extra arguments are refused.
To build just the example in a separate directory without tests:

```powershell
cmake -S . -B build/example-only -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/example-only --target controlled_market
.\build\example-only\controlled_market.exe 42
```

On Unix, run `./build/example-only/controlled_market 42`.
The example validates Phase 9 participants. The separate
[reference experiment](#run-the-reference-market-making-experiment) adds the
market maker, exclusive duration and exact spread report.

### Inspect controlled participant fills

```powershell
.\build\debug\controlled_market.exe --fills 42
$LASTEXITCODE
```

On Unix, use `./build/debug/controlled_market --fills 42`.
Omitting the seed uses 42, as in the other modes. It prints:

```text
participant_id=1 buy_fills=9 sell_fills=8 aggressive_fills=0 passive_fills=17
participant_id=2 buy_fills=17 sell_fills=14 aggressive_fills=3 passive_fills=28
participant_id=3 buy_fills=19 sell_fills=23 aggressive_fills=42 passive_fills=0
```

A fill counts one participant's leg of an execution, regardless of size or
whether its order becomes fully filled. A self-match counts one buy, one sell,
one aggressive and one passive leg for its owner. Across participants the
45 executions give 45 buys, 45 sells, 45 aggressive and 45 passive fills.
`participant_fills(ledger, id)` derives owned counts from committed history
without allocation or mutation. Unknown IDs return an empty optional.
See [fill-count semantics](docs/participant-accounting.md#participant-fill-counts).
`--events` and `--fills` are alternative modes; passing both returns exit code 2.

### Run the reference market-making experiment

`market_making` runs four participants for the exclusive logical window
[0, 500 ns): background liquidity, noise, market-order taker and a market maker.
The maker quotes at 99/101 ticks, size 5, refreshing every 25 ns.
Its explicit valuation mark is 100 ticks. Seed 42 is the default.

```powershell
cmake --build build/debug --target market_making
.\build\debug\market_making.exe 42
$LASTEXITCODE
.\build\debug\market_making.exe --events 42 | Set-Content -Encoding ascii -ErrorAction Stop .\build\debug\market-making-events.log
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/market_making 42
./build/debug/market_making --events 42 > build/debug/market-making-events.log
```

The summary has seven lines: totals, spread, four participant reports and depth.
Its first two lines are:

```text
seed=42 instrument_id=1 mark_ticks=100 horizon_ns=500 last_time_ns=496 requests=260 pending_requests=34 events=682 orders=186 cancellations=74 trades=99 volume_units=223 bid_ticks=97 ask_ticks=101
spread_tick_ns=1078 covered_ns=500 missing_ns=0 mean_spread_ticks=1078/500
```

The market maker ends with position +7, cash flow -577, realised PnL +116,
unrealised PnL +7 and total PnL +123, in exact tick-times-quantity units.
The spread is weighted by logical duration and kept as an exact fraction.
Time without both prices is reported separately. No request at 500 ns is
processed, no final flattening occurs, and observing through 500 ns leaves
the exchange's last committed time at 496 ns.

Custom seeds accept decimal digits from 0 through UINT64_MAX. Exit codes are
0 success, 1 run/output failure, 2 invalid arguments. `--events` exports the
existing v1 records instead of the summary.
See [the full reference contract and regression values](docs/reference-market-making.md).

For a clean standalone build without tests:

```powershell
cmake -S . -B build/reference-only -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/reference-only --target market_making
.\build\reference-only\market_making.exe 42
```

On Unix, run `./build/reference-only/market_making 42`.
All four examples remain available with `BUILD_TESTING=OFF`.
The reference is a correctness regression and future benchmark fixture.

### Run the multi-instrument scenario

`multi_instrument` drives ALPHA and BETA through the new single-writer router.
They share participant IDs and reuse client-order IDs while retaining independent
books, clocks, event counters, market data and accounts.

```powershell
cmake --build build/debug --target multi_instrument
.\build\debug\multi_instrument.exe
$LASTEXITCODE
.\build\debug\multi_instrument.exe --events | Set-Content -Encoding ascii -ErrorAction Stop .\build\debug\multi-instrument-events.log
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/multi_instrument
./build/debug/multi_instrument --events > build/debug/multi-instrument-events.log
```

The first summary line is `arrivals=12 routing_errors=1 events=25`.
ALPHA ends at 6 ns with 3 units traded; BETA ends at 5 ns with 7 units traded.
Both books are empty. At the explicit marks 101/201 ticks, the buyer's total
PnL is +3/+7 in the respective instrument's tick-times-quantity units.
ALPHA's tick is 0.01 and BETA's is 0.5; their raw tick notionals are kept separate.

The example has no seed argument. Exit codes are 0 success, 1 run/output failure
and 2 invalid arguments. `--events` prints 25 existing v1 records with
instrument-local sequences. One unknown destination produces a counted routing
diagnostic and no exchange record.
See [routing, identity and accounting scope](docs/multi-instrument-exchange.md).

For a clean standalone build without tests:

```powershell
cmake -S . -B build/multi-only -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/multi-only --target multi_instrument
.\build\multi-only\multi_instrument.exe
```

On Unix, run `./build/multi-only/multi_instrument`.

### Run the partitioned scenario

`partitioned_market` schedules ALPHA, BETA and GAMMA into private worker queues.
The default is two logical workers; choose 1 through 4 to compare partitions.
By default the runner is caller-driven. Add `--threaded` to execute the same
closed input batch with one thread per nonempty partition, joined before reports.

```powershell
cmake --build build/debug --target partitioned_market
.\build\debug\partitioned_market.exe
$LASTEXITCODE
.\build\debug\partitioned_market.exe 1
$LASTEXITCODE
.\build\debug\partitioned_market.exe 4
$LASTEXITCODE
.\build\debug\partitioned_market.exe 2 --events | Set-Content -Encoding ascii -ErrorAction Stop .\build\debug\partitioned-events.log
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/partitioned_market 2
./build/debug/partitioned_market 2 --events > build/debug/partitioned-events.log
```

With two workers the first line is
`workers=2 attempts=12 scheduled=11 refused=1 events=25`.
Worker 0 processes 7 arrivals and worker 1 processes 4. With four workers, the
counts are 4, 4, 3 and 0. The three instruments trade 3, 5 and 3 units respectively
and finish with empty books. Reports include each instrument's clock, counters,
positions, cash and total PnL at explicit marks 101/201/301 ticks.

Run the same scenario on actual threads:

```powershell
.\build\debug\partitioned_market.exe 2 --threaded
$LASTEXITCODE
.\build\debug\partitioned_market.exe 4 --threaded
$LASTEXITCODE
.\build\debug\partitioned_market.exe 2 --threaded --events | Set-Content -Encoding ascii -ErrorAction Stop .\build\debug\threaded-events.log
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/partitioned_market 2 --threaded
./build/debug/partitioned_market 2 --threaded --events > build/debug/threaded-events.log
```

The threaded summary starts with
`execution=threaded workers=2 attempts=12 scheduled=11 refused=1 events=25`.
The flags may appear in either order after the worker count; duplicates and
unknown flags return 2. Four configured workers use three threads for this script,
with the fourth worker idle. This example reports only a complete successful run;
a worker failure exits 1. The library's recoverable partial batches are documented
in [threaded simulation](docs/threaded-simulation.md).

Each instrument has identical events and final results for every supported
partition count. The combined event order follows ascending round-robin worker
steps in the default mode; threaded output is grouped by worker index, independent
of completion order. Both can differ between partition counts. Exit codes are 0 success,
1 run/output failure and 2 invalid arguments. See
[ownership, ordering and expected results](docs/partitioned-simulation.md).

For a clean standalone build without tests:

```powershell
cmake -S . -B build/partitioned-only -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/partitioned-only --target partitioned_market
.\build\partitioned-only\partitioned_market.exe 2
```

On Unix, run `./build/partitioned-only/partitioned_market 2`.

### Run the threaded batch tests

The threaded API has dedicated correctness and failure-recovery suites:

```powershell
cmake --build build/debug --target threaded_simulation_tests threaded_simulation_allocation_tests
ctest --test-dir build/debug --output-on-failure -R "^threaded_simulation(_allocation)?$"
```

Both tests are silent on success when run directly. Use `partitioned_market 2
--threaded` for the actual-thread example; its default mode remains caller-driven.
See [threaded configuration, publication and recovery](docs/threaded-simulation.md)
for the actual-thread API and its boundaries.

### Run an executable directly

The runnable targets exercise domain types, book operations, and raw order/cancel
processing. `simulation_delivery_tests` also runs a complete scripted timeline
and can print its events or market snapshots using the options below.
CTest is the usual way to run tests; you can also run any test directly after
building.

In Windows PowerShell:

```powershell
.\build\debug\price_tests.exe
$LASTEXITCODE
.\build\debug\aggregate_quantity_tests.exe
$LASTEXITCODE
.\build\debug\unsigned_product_tests.exe
$LASTEXITCODE
.\build\debug\cash_amount_tests.exe
$LASTEXITCODE
.\build\debug\domain_tests.exe
$LASTEXITCODE
.\build\debug\instrument_registry_tests.exe
$LASTEXITCODE
.\build\debug\worker_assignment_plan_tests.exe
$LASTEXITCODE
.\build\debug\multi_instrument_tests.exe
$LASTEXITCODE
.\build\debug\multi_instrument_allocation_tests.exe
$LASTEXITCODE
.\build\debug\multi_instrument_workload_tests.exe
$LASTEXITCODE
.\build\debug\partitioned_simulation_tests.exe
$LASTEXITCODE
.\build\debug\partitioned_simulation_allocation_tests.exe
$LASTEXITCODE
.\build\debug\partitioned_market_tests.exe
$LASTEXITCODE
.\build\debug\threaded_simulation_tests.exe
$LASTEXITCODE
.\build\debug\threaded_simulation_allocation_tests.exe
$LASTEXITCODE
.\build\debug\requests_tests.exe
$LASTEXITCODE
.\build\debug\orders_tests.exe
$LASTEXITCODE
.\build\debug\events_tests.exe
$LASTEXITCODE
.\build\debug\event_record_tests.exe
$LASTEXITCODE
.\build\debug\market_data_tests.exe
$LASTEXITCODE
.\build\debug\time_weighted_spread_tests.exe
$LASTEXITCODE
.\build\debug\participant_ledger_tests.exe
$LASTEXITCODE
.\build\debug\participant_ledger_allocation_tests.exe
$LASTEXITCODE
.\build\debug\participant_pnl_tests.exe
$LASTEXITCODE
.\build\debug\participant_fills_tests.exe
$LASTEXITCODE
.\build\debug\order_id_source_tests.exe
$LASTEXITCODE
.\build\debug\simulation_random_tests.exe
$LASTEXITCODE
.\build\debug\scripted_participant_tests.exe
$LASTEXITCODE
.\build\debug\participant_plans_tests.exe
$LASTEXITCODE
.\build\debug\participant_allocation_tests.exe
$LASTEXITCODE
.\build\debug\participant_integration_tests.exe
$LASTEXITCODE
.\build\debug\market_making_tests.exe
$LASTEXITCODE
.\build\debug\simulation_tests.exe
$LASTEXITCODE
.\build\debug\simulation_queue_tests.exe
$LASTEXITCODE
.\build\debug\simulation_queue_allocation_tests.exe
$LASTEXITCODE
.\build\debug\simulation_delivery_tests.exe
$LASTEXITCODE
.\build\debug\order_book_tests.exe
$LASTEXITCODE
.\build\debug\order_book_allocation_tests.exe
$LASTEXITCODE
.\build\debug\order_book_matching_tests.exe
$LASTEXITCODE
.\build\debug\order_book_limit_tests.exe
$LASTEXITCODE
.\build\debug\order_book_cancel_tests.exe
$LASTEXITCODE
.\build\debug\order_book_market_tests.exe
$LASTEXITCODE
.\build\debug\order_book_invariant_tests.exe
$LASTEXITCODE
.\build\debug\exchange_tests.exe
$LASTEXITCODE
.\build\debug\exchange_scenario_tests.exe
$LASTEXITCODE
.\build\debug\exchange_property_tests.exe
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/price_tests
echo $?
./build/debug/aggregate_quantity_tests
echo $?
./build/debug/unsigned_product_tests
echo $?
./build/debug/cash_amount_tests
echo $?
./build/debug/domain_tests
echo $?
./build/debug/instrument_registry_tests
echo $?
./build/debug/worker_assignment_plan_tests
echo $?
./build/debug/multi_instrument_tests
echo $?
./build/debug/multi_instrument_allocation_tests
echo $?
./build/debug/multi_instrument_workload_tests
echo $?
./build/debug/partitioned_simulation_tests
echo $?
./build/debug/partitioned_simulation_allocation_tests
echo $?
./build/debug/partitioned_market_tests
echo $?
./build/debug/threaded_simulation_tests
echo $?
./build/debug/threaded_simulation_allocation_tests
echo $?
./build/debug/requests_tests
echo $?
./build/debug/orders_tests
echo $?
./build/debug/events_tests
echo $?
./build/debug/event_record_tests
echo $?
./build/debug/market_data_tests
echo $?
./build/debug/time_weighted_spread_tests
echo $?
./build/debug/participant_ledger_tests
echo $?
./build/debug/participant_ledger_allocation_tests
echo $?
./build/debug/participant_pnl_tests
echo $?
./build/debug/participant_fills_tests
echo $?
./build/debug/order_id_source_tests
echo $?
./build/debug/simulation_random_tests
echo $?
./build/debug/scripted_participant_tests
echo $?
./build/debug/participant_plans_tests
echo $?
./build/debug/participant_allocation_tests
echo $?
./build/debug/participant_integration_tests
echo $?
./build/debug/market_making_tests
echo $?
./build/debug/simulation_tests
echo $?
./build/debug/simulation_queue_tests
echo $?
./build/debug/simulation_queue_allocation_tests
echo $?
./build/debug/simulation_delivery_tests
echo $?
./build/debug/order_book_tests
echo $?
./build/debug/order_book_allocation_tests
echo $?
./build/debug/order_book_matching_tests
echo $?
./build/debug/order_book_limit_tests
echo $?
./build/debug/order_book_cancel_tests
echo $?
./build/debug/order_book_market_tests
echo $?
./build/debug/order_book_invariant_tests
echo $?
./build/debug/exchange_tests
echo $?
./build/debug/exchange_scenario_tests
echo $?
./build/debug/exchange_property_tests
echo $?
```

Each test is silent on success and returns exit code `0`. On failure it prints a
diagnostic and returns a nonzero exit code.

### Release build and tests

Use a separate directory for the optimized build:

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

### Record the scripted timeline

The event writer is `write_event_record(output, event)` from
`<marketlab/event_record.hpp>`. Call it for returned events in their existing
order, then check the output stream. It writes exact decimal fields for all nine
types; malformed rejected requests retain their original numbers. The
[v1 format](docs/event-record-format.md) documents every field and I/O behavior.

The scripted integration executable can display its twelve validated events:

```powershell
cmake --build build/debug --target simulation_delivery_tests
.\build\debug\simulation_delivery_tests.exe --record-script
$LASTEXITCODE
```

To save them under the ignored build directory in PowerShell:

```powershell
.\build\debug\simulation_delivery_tests.exe --record-script | Set-Content -Encoding ascii -ErrorAction Stop .\build\debug\script-events.log
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/simulation_delivery_tests --record-script > build/debug/script-events.log
echo $?
```

Exit code 0 means the checks and output succeeded; unknown arguments return 2.
With no arguments, the executable remains silent on success. Its tests
compare recording enabled/disabled logical results and exact output lines.

### Inspect the scripted market snapshots

The same six-arrival scenario can display a market snapshot after each completed
step, including best prices, spread, exact mid, last trade, cumulative volume and
up to two price levels per side:

```powershell
cmake --build build/debug --target simulation_delivery_tests
.\build\debug\simulation_delivery_tests.exe --market-script
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/simulation_delivery_tests --market-script
echo $?
```

It prints six lines. The third has bid 100 / ask 101, spread 1 and mid 100.5.
The fourth reports the execution at price 101, size 2, trade time 3 and event
sequence 8. The last two retain that trade while cancellations advance snapshot
time/counters and remove the ask. Missing values print `none`; prices are integer
ticks and time is logical nanoseconds. `volume_units` is exact cumulative executed
size (0 for the first three observations, 2 thereafter). `bids` and `asks` list
`price_ticks:remaining_units:order_count`, separated by commas, or `none`. The
second observation contains `bids=100:8:2`; after the partial fill, the fourth has
`asks=101:3:1`. This is a readable scenario example; the
versioned event record remains the documented recording format.

All output modes run the normal integration checks first. Exit code 0 means
checks and output succeeded; 1 reports failure and 2 reports invalid arguments.
With no arguments, the executable remains silent on success. CTest compares all
six literal snapshot lines across twenty fresh replays and checks that observing
and recording the scenario preserve its event stream and final book.

### Inspect the scripted participant accounts

The same scenario can display its final quantity and cash-flow accounts:

```powershell
cmake --build build/debug --target simulation_delivery_tests
.\build\debug\simulation_delivery_tests.exe --account-script
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/simulation_delivery_tests --account-script
echo $?
```

It prints two lines. Participant 1 bought 2 units and has net position 2;
participant 2 sold 2 and has net position -2. `cash_tick_units` is -202 for the
buyer and 202 for the seller (2 units at 101 ticks); these are signed net flows,
not funded balances or rounded currency values. Both lines report one ledger
execution and last execution sequence 8. The exchange's later cancellation/status
events do not advance that execution-only ledger counter or change positions/cash.
The normal integration test checks these exact lines over twenty fresh replays
and reconstructs the accounts from retained execution history.

### Inspect total PnL at a chosen price

Provide an explicit positive whole-tick valuation price:

```powershell
cmake --build build/debug --target simulation_delivery_tests
.\build\debug\simulation_delivery_tests.exe --pnl-script 102
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/simulation_delivery_tests --pnl-script 102
echo $?
```

For the existing 2-unit trade at 101 ticks, this prints:

```text
instrument_id=1 participant_id=1 mark_ticks=102 total_pnl_tick_units=2
instrument_id=1 participant_id=2 mark_ticks=102 total_pnl_tick_units=-2
```

Total PnL is net execution cash plus signed position times the supplied price.
At mark 101 both results are zero; at mark 100 they are -2 and 2. Output uses
exact tick-times-quantity units. The command chooses no price from market data
and does not split realised/unrealised PnL. Use decimal digits from 1 through
INT64_MAX; missing, zero, negative, fractional, out-of-range or trailing-text
marks return exit code 2 with usage. Success returns 0.

### Inspect FIFO realised and unrealised PnL

Use the same explicit positive whole-tick price with the FIFO projection:

```powershell
cmake --build build/debug --target simulation_delivery_tests
.\build\debug\simulation_delivery_tests.exe --fifo-pnl-script 102
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/simulation_delivery_tests --fifo-pnl-script 102
echo $?
```

It prints realised, unrealised and total PnL in exact tick-times-quantity units:

```text
instrument_id=1 participant_id=1 mark_ticks=102 realised_pnl_tick_units=0 unrealised_pnl_tick_units=2 total_pnl_tick_units=2
instrument_id=1 participant_id=2 mark_ticks=102 realised_pnl_tick_units=0 unrealised_pnl_tick_units=-2 total_pnl_tick_units=-2
```

The script leaves both positions open, so its PnL is entirely unrealised.
FIFO closes oldest opposite lots when later executions reduce a position;
self-matches do not change those lots. The query reconstructs temporary lots
from execution history and leaves the ledger unchanged. The same mark validation
and exit codes as `--pnl-script` apply. See the
[accounting examples](docs/participant-accounting.md#fifo-realisedunrealised-decomposition)
for partial closes, reversals and nonzero realised PnL.

### Reproduce a generated workload

Run one seed with a chosen request count (decimal seed 0 through UINT64_MAX;
request count 1 through 1,000,000):

```powershell
.\build\debug\exchange_property_tests.exe 42 100000
$LASTEXITCODE
```

In a Unix shell, use `./build/debug/exchange_property_tests 42 100000`.
Success is silent with exit code 0. Failure returns 1 and prints the seed,
one-based failing request number, replay arguments and the complete raw request
prefix. Replaying a seed requires the same engine/test version; the printed
requests retain exact IDs, fields and timestamps for inspection. Invalid CLI
arguments return 2 with usage.

An optional third argument alters the first returned execution at that request,
without changing production code. This verifies failure reporting and replay:

```powershell
.\build\debug\exchange_property_tests.exe 42 256 4
$LASTEXITCODE
```

This control intentionally exits 1 at request 4 with an execution-quantity
reconciliation error. Selecting a request without an execution also exits 1
with a diagnostic. The regular CTest target verifies this failure internally
and succeeds only when its seed/request replay produces the same error and prefix.
These generated workloads validate correctness.

### GCC checked-container tests

GCC/libstdc++ can also check container and iterator operations at runtime in a
separate build. These commands build and run forty-one tests covering threaded/partitioned simulation, worker
assignment, controlled participants, accounting,
exact aggregates, market data, recording, simulation, books and the exchange:

```powershell
cmake -S . -B build/checked -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS=-D_GLIBCXX_DEBUG
cmake --build build/checked --target instrument_registry_tests worker_assignment_plan_tests multi_instrument_tests multi_instrument_allocation_tests multi_instrument_workload_tests partitioned_simulation_tests partitioned_simulation_allocation_tests partitioned_market_tests threaded_simulation_tests threaded_simulation_allocation_tests order_id_source_tests simulation_random_tests scripted_participant_tests participant_plans_tests participant_allocation_tests participant_integration_tests market_making_tests time_weighted_spread_tests unsigned_product_tests cash_amount_tests participant_ledger_tests participant_ledger_allocation_tests participant_pnl_tests participant_fills_tests aggregate_quantity_tests event_record_tests market_data_tests simulation_tests simulation_queue_tests simulation_queue_allocation_tests simulation_delivery_tests order_book_tests order_book_allocation_tests order_book_matching_tests order_book_limit_tests order_book_cancel_tests order_book_market_tests order_book_invariant_tests exchange_tests exchange_scenario_tests exchange_property_tests
ctest --test-dir build/checked --output-on-failure -R "^(instrument_registry|worker_assignment_plan|multi_instrument(_allocation|_workload)?|partitioned_(simulation(_allocation)?|market)|threaded_simulation(_allocation)?|order_id_source|simulation_random|scripted_participant|participant_(plans|allocation|integration)|unsigned_product|cash_amount|participant_ledger(_allocation)?|participant_pnl|participant_fills|aggregate_quantity|event_record|market_data|time_weighted_spread|market_making|simulation(_queue(_allocation)?|_delivery)?|order_book(_allocation|_matching|_limit|_cancel|_market|_invariant)?|exchange(_scenario|_property)?)$"
```

Keep this build separate: the flag changes standard-container layouts, so code
sharing containers must use the same mode. See the
[libstdc++ debug-mode documentation](https://gcc.gnu.org/onlinedocs/libstdc++/manual/debug_mode_using.html).

### Linux ThreadSanitizer tests

Run the concurrency suites under ThreadSanitizer on Linux or Linux within WSL.
The current MinGW compiler does not provide its runtime. A Linux compiler with
C++23, pthreads and ThreadSanitizer is required; GCC 16.2 was validated under WSL.
No Linux CMake or Ninja installation is needed for this focused check.

From the repository root in a Linux shell:

```sh
sh scripts/test-thread-sanitizer.sh
```

From PowerShell at the repository root, with a configured WSL distribution:

```powershell
wsl --exec sh scripts/test-thread-sanitizer.sh
$LASTEXITCODE
```

The script compiles and runs `threaded_simulation_tests`,
`threaded_simulation_allocation_tests` and `partitioned_market_tests` with
`-O1 -g -fsanitize=thread -fno-omit-frame-pointer -pthread`, treating compiler
warnings as errors. `CXX` can select another compatible compiler executable.
Binaries stay under ignored `build/tsan`; the script works when invoked by an
absolute path from another directory. Git keeps this script's line endings LF.

A compile/test error stops the script immediately. Each test runs with
`TSAN_OPTIONS=halt_on_error=1:exitcode=66`; no race suppressions are configured.
Exit 0 means all three suites passed without a sanitizer report; unsupported
non-Linux shells return 2. Missing compiler/runtime errors remain visible.
These checks exercise the closed-batch API, failure recovery and threaded CLI;
live ingress is outside their scope. See the
[GCC instrumentation options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html).

MarketLab is simulation infrastructure and does not place real trades.
