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
scheduled request records, passive order-book storage, and single-match limit fills.
Logical timestamps count unsigned 64-bit nanoseconds from the simulation origin.
Tick sizes use a positive decimal coefficient and a scale from 0 through 18;
instrument configuration requires a nonzero ID and a nonempty symbol.

For v0, a participant is represented by its `ParticipantId`. Orders and executions
carry that identity directly. Participant registration will belong to exchange
configuration; strategy behavior, positions, cash, and PnL are later components.

Submitted order quantities must be positive; zero is supported for remaining
quantities after a complete fill. Requests retain raw quantity and tick-price
values so invalid submissions can be rejected explicitly. `validate_fields()`
checks numeric IDs, side/type, quantity, and price fields in the specified order
and returns an optional rejection reason. Registration, duplicates, clock
ordering, active-order lookup, and cancellation ownership require the future
exchange core; passing field validation alone does not accept an order.

Accepted orders retain their original quantity and request sequence for arrival
priority. Resting records require a limit order and a positive remainder no
greater than its original quantity. Executions carry both order IDs, buyer and
seller IDs, the aggressive side, exact price and quantity, logical time, and
distinct request/event sequences. Record `is_valid()` checks intrinsic consistency;
the future exchange must establish acceptance, book membership, and matching
correctness. One execution represents one trade, including permitted self-matches.

`ExchangeEvent` is a `std::variant` of the nine contract events: acceptance,
execution, partial fill, full fill, resting, market-remainder expiry, cancellation,
order rejection, and cancel rejection. Successful records require valid numeric
IDs and quantities consistent with their status. Rejections retain the submitted
request, including invalid fields, and its rejection reason. Backward-time
rejections preserve the submitted timestamp while carrying current logical time.
These are data records; event production, reason selection, and ordered emission
will belong to the matching core.

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

The caller supplies the execution sequence. A future exchange will validate and
process each entire request atomically, allocate unique sequences, record executions,
emit ordered events, and continue matching before resting or expiring the remainder.
Whole-request matching, market orders, cancellation, exchange acceptance, and the
decimal price adapter remain future work.

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
Currently there are nine registered tests: `price`, `domain`, `requests`, `orders`,
`events`, `simulation`, `order_book`, `order_book_allocation`, and `order_book_matching`.
`--output-on-failure` prints diagnostics from failing tests, and CTest returns a
nonzero exit code on failure.

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

The runnable targets exercise domain types, storage, and single matches. A simulation executable
will be added when the exchange core exists. CTest is the usual way to run tests;
you can also run any test directly after building.

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
separate build. These commands build and run only the three order-book tests:

```powershell
cmake -S . -B build/checked -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS=-D_GLIBCXX_DEBUG
cmake --build build/checked --target order_book_tests order_book_allocation_tests order_book_matching_tests
ctest --test-dir build/checked --output-on-failure -R "^order_book(_allocation|_matching)?$"
```

Keep this build separate: the flag changes standard-container layouts, so code
sharing containers must use the same mode. See the
[libstdc++ debug-mode documentation](https://gcc.gnu.org/onlinedocs/libstdc++/manual/debug_mode_using.html).

MarketLab is simulation infrastructure and does not place real trades.
