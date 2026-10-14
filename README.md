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

## Build, run, and test

The current C++23 components are exact integer-tick prices, nonnegative
quantities with checked subtraction, distinct order/participant/instrument IDs,
order sides/types, logical timestamps, instrument tick configuration,
order/cancel requests, accepted/resting orders, executions, exchange events,
scheduled request records, passive order-book storage, single-match limit fills,
read-only limit match counting, complete accepted-order processing for limits and
markets, active-order lookup, owned-order cancellation, and raw order/cancel
processing through a single-instrument exchange.
Logical timestamps count unsigned 64-bit nanoseconds from the simulation origin.
Tick sizes use a positive decimal coefficient and a scale from 0 through 18;
instrument configuration requires a nonzero ID and a nonempty symbol.

For v0, a participant is represented by its `ParticipantId`. Orders and executions
carry that identity directly. Participant registration belongs to fixed
`Exchange` configuration; strategy behavior, positions, cash, and PnL are later components.

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
contain malformed requests for the exchange to reject. The event queue, sequence
assignment, and latency handling remain future work.

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
Recording, scheduling, and the decimal price adapter remain future work.

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
Currently there are fourteen registered tests: `price`, `domain`, `requests`, `orders`,
`events`, `simulation`, `order_book`, `order_book_allocation`, `order_book_matching`,
`order_book_limit`, `order_book_cancel`, `order_book_market`, `exchange`, and
`exchange_scenario`.
`--output-on-failure` prints diagnostics from failing tests, and CTest returns a
nonzero exit code on failure.

`exchange_scenario` runs all 23 canonical rows from the matching contract through
the raw exchange entry points. It checks complete ordered event records, final
active orders and FIFO heads, original order metadata, time/counters, and preserved
previously returned events. The existing tests retain boundary and allocation-failure
coverage; the full invariant and generated-workload gates remain future work.

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
processing. A simulation executable will be added with end-to-end scenarios.
CTest is the usual way to run tests; you can also run any test directly after
building.

In Windows PowerShell:

```powershell
.\build\debug\price_tests.exe
$LASTEXITCODE
.\build\debug\domain_tests.exe
$LASTEXITCODE
.\build\debug\requests_tests.exe
$LASTEXITCODE
.\build\debug\orders_tests.exe
$LASTEXITCODE
.\build\debug\events_tests.exe
$LASTEXITCODE
.\build\debug\simulation_tests.exe
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
.\build\debug\exchange_tests.exe
$LASTEXITCODE
.\build\debug\exchange_scenario_tests.exe
$LASTEXITCODE
```

In a Unix shell:

```sh
./build/debug/price_tests
echo $?
./build/debug/domain_tests
echo $?
./build/debug/requests_tests
echo $?
./build/debug/orders_tests
echo $?
./build/debug/events_tests
echo $?
./build/debug/simulation_tests
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
./build/debug/exchange_tests
echo $?
./build/debug/exchange_scenario_tests
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

### GCC checked-container tests

GCC/libstdc++ can also check container and iterator operations at runtime in a
separate build. These commands build and run the eight book/exchange tests:

```powershell
cmake -S . -B build/checked -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS=-D_GLIBCXX_DEBUG
cmake --build build/checked --target order_book_tests order_book_allocation_tests order_book_matching_tests order_book_limit_tests order_book_cancel_tests order_book_market_tests exchange_tests exchange_scenario_tests
ctest --test-dir build/checked --output-on-failure -R "^(order_book(_allocation|_matching|_limit|_cancel|_market)?|exchange(_scenario)?)$"
```

Keep this build separate: the flag changes standard-container layouts, so code
sharing containers must use the same mode. See the
[libstdc++ debug-mode documentation](https://gcc.gnu.org/onlinedocs/libstdc++/manual/debug_mode_using.html).

MarketLab is simulation infrastructure and does not place real trades.
