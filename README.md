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

Phase 8 accounting now tracks execution history, bought/sold volumes, exact net
positions and cash flows in a separate participant ledger. Realised and
mark-to-market PnL remain later parts of this phase. See
[participant accounting](docs/participant-accounting.md) for the implemented
contract and examples.

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

For v0, a participant is represented by its `ParticipantId`. Orders and executions
carry that identity directly. Participant registration belongs to fixed
`Exchange` configuration. `ParticipantLedger` owns downstream execution history,
gross volumes, net positions and cash flows; strategy behavior and PnL remain later work.

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
returning an empty optional if the product exceeds 192 bits. Funding, solvency
checks, fees and PnL are outside this cash-flow API.

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
Participant behavior, modeled latency and the decimal price adapter
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
Currently there are twenty-six registered tests: `price`, `aggregate_quantity`,
`unsigned_product`, `cash_amount`,
`domain`, `requests`, `orders`,
`events`, `event_record`, `market_data`, `participant_ledger`,
`participant_ledger_allocation`, `simulation`, `simulation_queue`,
`simulation_queue_allocation`,
`simulation_delivery`, `order_book`, `order_book_allocation`, `order_book_matching`,
`order_book_limit`, `order_book_cancel`, `order_book_market`, `order_book_invariant`,
`exchange`, `exchange_scenario`, and `exchange_property`.
`--output-on-failure` prints diagnostics from failing tests, and CTest returns a
nonzero exit code on failure.

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
checked against independent decimal arithmetic, including overflow.

`participant_ledger` checks initial state, account snapshots, signed positions
and cash flows,
self-matches, invalid/order/registration refusal, whole-batch rollback, correction,
full and incremental replay, quantity totals beyond 64 bits and cash beyond
128 bits. `participant_ledger_allocation` injects each map/history allocation failure, preserves prior history views and
verifies one-time retry from retained, already committed simulation executions.
Generated workloads also compare each account to independent decimal arithmetic
and replay their full execution history.

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
.\build\debug\participant_ledger_tests.exe
$LASTEXITCODE
.\build\debug\participant_ledger_allocation_tests.exe
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
./build/debug/participant_ledger_tests
echo $?
./build/debug/participant_ledger_allocation_tests
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
separate build. These commands build and run twenty-one tests covering accounting,
exact aggregates, market data, recording, simulation, books and the exchange:

```powershell
cmake -S . -B build/checked -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS=-D_GLIBCXX_DEBUG
cmake --build build/checked --target unsigned_product_tests cash_amount_tests participant_ledger_tests participant_ledger_allocation_tests aggregate_quantity_tests event_record_tests market_data_tests simulation_tests simulation_queue_tests simulation_queue_allocation_tests simulation_delivery_tests order_book_tests order_book_allocation_tests order_book_matching_tests order_book_limit_tests order_book_cancel_tests order_book_market_tests order_book_invariant_tests exchange_tests exchange_scenario_tests exchange_property_tests
ctest --test-dir build/checked --output-on-failure -R "^(unsigned_product|cash_amount|participant_ledger(_allocation)?|aggregate_quantity|event_record|market_data|simulation(_queue(_allocation)?|_delivery)?|order_book(_allocation|_matching|_limit|_cancel|_market|_invariant)?|exchange(_scenario|_property)?)$"
```

Keep this build separate: the flag changes standard-container layouts, so code
sharing containers must use the same mode. See the
[libstdc++ debug-mode documentation](https://gcc.gnu.org/onlinedocs/libstdc++/manual/debug_mode_using.html).

MarketLab is simulation infrastructure and does not place real trades.
