# Exchange benchmark baseline

The first phase-13 benchmark measures one `Exchange`, one instrument and two
registered participants, on the calling thread. It uses the existing matching
contract. It does not change the exchange or implement an optimization.

## Build and run

Benchmarks are opt-in. Use a separate Release directory:

```powershell
cmake -S . -B build/benchmark -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DMARKETLAB_BUILD_BENCHMARKS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/benchmark --target exchange_benchmark
.\build\benchmark\exchange_benchmark.exe match 100000
.\build\benchmark\exchange_benchmark.exe cancel 100000
.\build\benchmark\exchange_benchmark.exe sweep 100000 --record
```

Unix uses the same CMake commands and `./build/benchmark/exchange_benchmark`.
The CLI accepts `match`, `cancel` or `sweep`, an optional cycle count (default
10,000; range 1 through 1,000,000), and optional `--record`. Count and flag can
appear in either order. `--help` prints usage. Exit codes are 0 success, 1
processing/output/check failure and 2 invalid arguments. Each successful run
prints one JSON object using schema `marketlab.exchange-benchmark.v1`.

## Exact workloads

Each process starts with an empty book and fresh lifetime ID history. All orders
are limits, timestamps are increasing request indexes from zero, and new orders
use fresh positive IDs. There is no randomness or modeled latency.

| Scenario | One cycle | Requests | Events | Executions | Traded units |
|---|---|---:|---:|---:|---:|
| `match` | Participant 1 sells 10 at 100; participant 2 buys all 10 at 100 | 2 | 6 | 1 | 10 |
| `cancel` | Participant 1 rests a bid of 10 at 100, then cancels it | 2 | 3 | 0 | 0 |
| `sweep` | Participant 1 rests asks of 1 at 100, 2 at 101 and 3 at 102; participant 2 buys 6 with limit 102 | 4 | 16 | 3 | 6 |

Prices are exact ticks. Each cycle ends with an empty book. Accepted-ID history
continues to grow within a process, so different cycle counts describe different
history/allocator conditions. This measures full exchange calls, including raw
validation, IDs, matching and event construction; it is not an isolated book-only
matching measurement. Multi-instrument/worker baselines remain later phase-13 work.

## What the clocks include

Requests, exchange construction, latency storage and the recorder sink are
prepared before timing. The reported `wall_ns` covers the processing loop:
dispatch, exchange calls, clock instrumentation, event counting, optional
recording and per-request event-vector destruction. Throughput is request,
execution or event count divided by that loop duration.

`process_ns` measures each dispatched exchange call through event-vector return.
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
and counters, cumulative volume and last execution terms. A failed check exits 1
instead of publishing a successful sample. Sorting latency samples, checks,
JSON output, exchange teardown and process startup are outside `wall_ns`.

## Collect a reproducible Windows run

From the repository root, with the Release configuration above:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-exchange-benchmarks.ps1 -Cycles 100000 -Repetitions 5
```

The execution policy applies to this process only. The collector needs Windows
CIM access to read CPU/OS/RAM metadata; restricted environments must allow those
read-only queries. It rebuilds the target, requires exported compile commands,
then runs one discarded warmup process and five fresh measured processes for
each of the six scenario/recording combinations. Every measured repetition is
saved; deterministic counters and record hashes must agree with the warmup.

The printed path names an ignored `build/benchmark/results-<UTC>.json` file with
schema `marketlab.exchange-benchmark-run.v1`. It records CPU/core information,
physical RAM, OS/version/architecture, exact compiler command and directory,
compiler/build configuration, Git commit/dirty state, worker/instrument counts,
null seed, OS-default affinity, cycle/repetition/warmup counts, executable and
argument arrays, and all raw sample metrics. `-OutputPath` selects another file
and overwrites that explicit path. A clean committed tree identifies shareable
source; dirty state is recorded rather than hidden. Repeat the exact configuration
when comparing runs, and inspect the raw variation rather than treating one
sample as a regression threshold.

On other systems, run the executable directly and capture equivalent hardware,
OS, compiler flags, source and command metadata before sharing measurements.
Peak process memory and worker utilization are not measured by this first lot.
Physical RAM in the collector describes hardware, not benchmark memory usage.

## Correctness tests

The normal test build includes `exchange_benchmark_tests` even when benchmark
executables are disabled. It checks small workloads, three-level sweep inputs,
nearest-rank boundaries, a known FNV vector and complete encoded event bytes
against an independent stream. Timing values have no performance assertions.

```powershell
cmake --build build/debug --target exchange_benchmark_tests
ctest --test-dir build/debug --output-on-failure -R "^exchange_benchmark$"
```
