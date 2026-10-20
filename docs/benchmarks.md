# Exchange and order-book benchmarks

The phase-13 benchmark measures one `Exchange`, its direct `OrderBook` API, or
single-threaded routing through `MultiInstrumentExchange`. All modes use the
existing matching contract and the same two participant IDs. No engine
optimization is included.

## Build and run

Benchmarks are opt-in. Use a separate Release directory:

```powershell
cmake -S . -B build/benchmark -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DMARKETLAB_BUILD_BENCHMARKS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/benchmark --target exchange_benchmark
.\build\benchmark\exchange_benchmark.exe match 100000
.\build\benchmark\exchange_benchmark.exe cancel 100000
.\build\benchmark\exchange_benchmark.exe sweep 100000 --record
.\build\benchmark\exchange_benchmark.exe match 100000 --book
.\build\benchmark\exchange_benchmark.exe sweep 100000 --book --record
.\build\benchmark\exchange_benchmark.exe match 100000 --instruments 16
.\build\benchmark\exchange_benchmark.exe sweep 100000 --instruments 16 --skewed --record
```

Unix uses the same CMake commands and `./build/benchmark/exchange_benchmark`.
The CLI accepts `match`, `cancel` or `sweep`, an optional cycle count (default
10,000; range 1 through 1,000,000), optional `--record`, and either `--book` or
`--instruments N` (1 through 256). Routed mode also accepts `--skewed`, requiring
at least two instruments. Without either owner option, it measures one exchange.
Count and flags can appear in any order; duplicate flags/counts are refused.
`--help` prints usage. Exit codes are 0 success, 1
processing/output/check failure and 2 invalid arguments. Each successful run
prints one JSON object using schema `marketlab.exchange-benchmark.v1`.
The additive `engine` field identifies `exchange` (default), `book` or `routed`.
`traffic` identifies `single`, `balanced` or `hot`; routed results also include
`instrument_cycles` in increasing instrument-ID order, including idle books.

## Exact workloads

Each process starts with an empty book; exchange mode has fresh lifetime ID
history. All orders are limits, timestamps are increasing request indexes from zero, and new orders
use fresh positive IDs. There is no randomness or modeled latency.

| Scenario | One cycle | Requests | Events | Executions | Traded units |
|---|---|---:|---:|---:|---:|
| `match` | Participant 1 sells 10 at 100; participant 2 buys all 10 at 100 | 2 | 6 | 1 | 10 |
| `cancel` | Participant 1 rests a bid of 10 at 100, then cancels it | 2 | 3 | 0 | 0 |
| `sweep` | Participant 1 rests asks of 1 at 100, 2 at 101 and 3 at 102; participant 2 buys 6 with limit 102 | 4 | 16 | 3 | 6 |

Prices are exact ticks. Each cycle ends with an empty book. In default exchange
mode, accepted-ID history continues to grow within a process, so different cycle
counts describe different history/allocator conditions. This mode measures full
exchange calls, including raw validation, IDs, matching and event construction.

With `--book`, trusted `AcceptedOrder` records are prepared before timing from
the same fixed valid requests. Arrival sequences are request index + 1; the
caller supplies contiguous event sequences to `process_order()` and `cancel()`.
There is no exchange owner, registration check, lifetime-ID history or exchange
market-data update in this mode. The book still performs its own validation,
storage, matching and event construction. Cancellation returns its single record
without allocating an event vector. This measures the complete book API, including
passive insertion, rather than only the allocation-free `match_one()` kernel.
Identical scenarios must yield identical encoded event streams in both modes.

## Routed workloads

`--instruments N` constructs a registry and one exchange per instrument, with
IDs 1 through N, a one-tick tick size and both participants registered on every
instrument. One calling thread routes every request through the existing
`MultiInstrumentExchange` API. Even N=1 uses the router, providing a control
against the original single-exchange workload.

The cycle count is the total across all instruments. Each complete cycle from
the table above stays on one instrument, with the same prices, quantities,
participants, globally increasing request-index timestamps and fresh order IDs.
Cycles run successively; the fixture does not maintain simultaneous resting
liquidity across books. Every exchange retains its own sequences, clock and
lifetime-ID history. No random draw, global exchange sequence or thread is added.

Balanced traffic assigns cycle c (starting at zero) to instrument `1 + c % N`.
With `--skewed`, the first nine cycles of each block of ten target instrument 1;
the tenth targets `2 + (c / 10) % (N - 1)`, using integer division. Cold cycles
rotate through instruments 2 through N. Thus a complete block is exactly 90%
hot traffic; partial blocks and low cycle counts may leave instruments idle.
For N=4 and 40 cycles, balanced loads are `[10,10,10,10]` and hot loads are
`[36,2,1,1]`. Published `instrument_cycles` gives the actual finite distribution.

Preparation of requests, owners, expected loads and latency storage is outside
timing. `process_ns` includes variant dispatch, router lookup, exchange processing
and result return. The timed loop also counts outcomes and optionally records
events. After timing, aggregate counts/notional and each instrument's empty-book
invariants, local request/event counters, final time, cumulative volume and last
execution terms are checked, including zero state on idle instruments. Latencies
are pooled across requests in this process; they are not per-instrument quantiles.
Worker scaling, queueing and concurrent ingress remain separate phase-13 work.

## What the clocks include

Requests, selected owner construction, accepted book inputs, latency storage and
the recorder sink are prepared before timing. The reported `wall_ns` covers the processing loop:
dispatch, selected API calls, clock instrumentation, event counting, optional
recording and per-request event-vector destruction. Throughput is request,
execution or event count divided by that loop duration.

`process_ns` measures each dispatched call through its result return.
It excludes subsequent event counting, recording and vector destruction. p50,
p95, p99 and p99.9 use exact nearest ranks (`ceil(p * N)`), with no interpolation;
min/max are included. Small samples naturally give the same upper percentiles.
Clock granularity and instrumentation affect these samples; there is no latency
threshold in the tests and no subtraction of an estimated clock cost.

With `--record`, the real v1 text encoder writes all events to a constant-memory
sink that counts bytes and computes FNV-1a-64. Recorder-on timing includes that
hashing work. It measures text-plus-hash processing, not filesystem durability
or disk throughput. Recording off produces zero bytes and a null record hash.

After timing, each run verifies request/event/execution counts, traded quantity
and tick-times-quantity notional, no rejections, empty valid books, final clock
and counters, cumulative volume and last execution terms. Exchange clock/counter/
volume projections are checked only in exchange mode; book mode checks the
returned execution terms and counted totals. A failed check exits 1
instead of publishing a successful sample. Sorting latency samples, checks,
JSON output, owner teardown and process startup are outside `wall_ns`.

## Collect a reproducible Windows run

From the repository root, with the Release configuration above:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-exchange-benchmarks.ps1 -Cycles 100000 -Repetitions 5
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-exchange-benchmarks.ps1 -Cycles 100000 -Repetitions 5 -Engine all
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-exchange-benchmarks.ps1 -Cycles 100000 -Repetitions 5 -Engine routed -Instruments 16 -Traffic all
```

The execution policy applies to this process only. The collector needs Windows
CIM access to read CPU/OS/RAM metadata; restricted environments must allow those
read-only queries. It rebuilds the target, requires exported compile commands,
then runs one discarded warmup process and five fresh measured processes for
each of the six scenario/recording combinations. Every measured repetition is
saved; deterministic counters and record hashes must agree with the warmup.
The default `-Engine exchange` collects six variants. `-Engine book` collects
the six direct-book variants; `-Engine all` collects all twelve, retaining 60
samples at five repetitions. Each variant receives its own discarded warmup.
`-Engine routed` collects six variants for `-Traffic balanced` (the default) or
`-Traffic hot`, and twelve for `-Traffic all`. `-Instruments` defaults to 16 in
this mode. Both parameters require `-Engine routed`; hot traffic with N=1 is
refused. Existing `-Engine all` continues to mean exchange plus book. Routed
collection validates and saves per-instrument counts and exact routing arguments.

The printed path names an ignored `build/benchmark/results-<UTC>.json` file with
schema `marketlab.exchange-benchmark-run.v1`. It records CPU/core information,
physical RAM, OS/version/architecture, exact compiler command and directory,
compiler/build configuration, Git commit/dirty state, worker/instrument counts,
null seed, OS-default affinity, cycle/repetition/warmup counts, executable and
argument arrays, selected engines/traffic patterns and all raw sample metrics. `-OutputPath`
selects another file and overwrites that explicit path. A clean committed tree identifies shareable
source; dirty state is recorded rather than hidden. Repeat the exact configuration
when comparing runs, and inspect the raw variation rather than treating one
sample as a regression threshold.

On other systems, run the executable directly and capture equivalent hardware,
OS, compiler flags, source and command metadata before sharing measurements.
Worker utilization remains unmeasured. Physical RAM in the collector describes
hardware; per-process memory appears separately in each sample.

## Process memory

Each successful run reads `memory.peak_resident_bytes` after workload processing,
validation and owner teardown, before JSON output. The read is outside `wall_ns`
and `process_ns`. It is the process high-water mark up to that point: input and
latency buffers, owner state/history, event storage, invariant checks, runtime,
libraries and allocator behavior can all contribute. Book mode retains both raw
and accepted input buffers during processing. The value is not a timed-loop-only
allocation count, isolated engine footprint, private heap size or machine RAM.

On Windows, `memory.method` is `windows_peak_working_set`, using the byte-valued
`PeakWorkingSetSize` from
[GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo)
and [PROCESS_MEMORY_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters).
The opt-in executable links the system Psapi library; core and test targets do
not acquire that dependency. On Linux, the method is `linux_getrusage_maxrss`:
[`getrusage(RUSAGE_SELF)`](https://man7.org/linux/man-pages/man2/getrusage.2.html)
reports KiB, converted to bytes with checked bounds. These are OS-specific
resident-memory metrics; compare runs within the same OS/method and build.

Other platforms emit a null value and method `unavailable`. On supported
platforms, a failed query exits 1 before publishing JSON. The Windows collector
requires a positive Windows measurement and retains it for every sample; memory
values may vary across repetitions and are not compared as deterministic fields.
Earlier JSON artifacts without `memory` contain no process-memory measurement.
No memory-size threshold is enforced by correctness tests.

The optional CLI test parses all three scenarios across exchange, book and
routed modes and validates the platform's memory method and JSON value:

```powershell
cmake -S . -B build/benchmark-check -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMARKETLAB_BUILD_BENCHMARKS=ON
cmake --build build/benchmark-check --target exchange_benchmark
ctest --test-dir build/benchmark-check --output-on-failure -R "^exchange_benchmark_cli$"
```

## Correctness tests

The normal test build includes `exchange_benchmark_tests` even when benchmark
executables are disabled. It checks small workloads, three-level sweep inputs,
nearest-rank boundaries, a known FNV vector, literal workload totals and complete
encoded event bytes against an independent stream. Book/exchange equivalence
covers all three scenarios, recording settings and multiple cycles, including
cancellation sequence continuity. Timing values have no performance assertions.
Routed tests verify literal 40-cycle distribution fixtures, instrument limits,
idle books and event-stream equivalence with isolated exchanges over the three
scenarios, both traffic patterns and 1, 4, 16 and 256 instruments. N=1 routing
also matches the original single-exchange recording exactly.

```powershell
cmake --build build/debug --target exchange_benchmark_tests
ctest --test-dir build/debug --output-on-failure -R "^exchange_benchmark$"
```
