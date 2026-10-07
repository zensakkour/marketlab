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
order sides/types, logical timestamps, instrument tick configuration, and
order/cancel request data, accepted/resting order records, executions, and
exchange-event records.
Logical timestamps count unsigned 64-bit nanoseconds from the simulation origin.
Tick sizes use a positive decimal coefficient and a scale from 0 through 18;
instrument configuration requires a nonzero ID and a nonempty symbol.

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

The matching contract is in [docs/matching-semantics.md](docs/matching-semantics.md).
The order book and decimal price adapter are not implemented yet.

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
Currently there are five registered tests: `price`, `domain`, `requests`, `orders`,
and `events`.
`--output-on-failure` prints diagnostics from failing tests, and CTest returns a
nonzero exit code on failure.

After editing C++ code, rerun the build and CTest commands. Rerun configuration
after changing `CMakeLists.txt` or build options.

### Run an executable directly

The runnable targets currently exercise the domain types. A simulation executable
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

MarketLab is simulation infrastructure and does not place real trades.
