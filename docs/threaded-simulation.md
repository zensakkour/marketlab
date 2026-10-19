# Threaded closed-batch simulation

`ThreadedSimulation` in `<marketlab/threaded_simulation.hpp>` runs independent
instrument partitions on actual worker threads. It uses the same private
worker processing as the [caller-driven runner](partitioned-simulation.md),
with the same exact matching, local clocks, queue ordering and failure-atomic
individual arrivals.

## Configure, schedule, run

Construct with `ThreadedSimulation(registry, plan, participants)`. Configuration
validation, fixed participant registration, copied input ownership and sparse
idle worker indexes follow `PartitionedSimulation`.

The coordinator calls `schedule(request)` before `run()`. Scheduling returns
the existing `PartitionedScheduleError` diagnostics and worker-local insertion
sequences. Unknown destinations and times before that instrument's committed
clock consume no sequence. Other malformed fields remain for exchange validation.

`run()` closes the current input set for processing and launches one
`std::jthread` for each partition with pending arrivals. Every thread exclusively
mutates that partition's exchanges and queue; the assignment cannot change.
Idle/drained partitions launch no thread and are absent from the returned vector.
All writers join before `run()` returns. No thread survives a call.

All public calls on this owner must be serialized. During `run()`, the caller
must not schedule, invoke another run, destroy the owner or read borrowed
exchange/queue views from another thread. There is no live ingress, asynchronous
observer, callback, cancellation or overlapping-batch API.

`plan()`, `find(id)` and `queue(index)` expose the same non-allocating const
views as the caller-driven runner. Their object addresses stay valid until owner
destruction. Observe them only between runs, and reacquire queue heads after
successful scheduling/processing.

## Publication and results

`run()` returns `vector<WorkerBatch>` in increasing worker-index order.
Within each batch, `steps` follows its queue's timestamp/insertion order.
Thread completion order never determines publication order. Event sequences
remain instrument-local, and arrival sequences remain worker-local.

Each `WorkerBatch` owns:

- `worker_index`: the original configured owner index;
- `steps`: every committed `WorkerStep`, including order/cancel rejections;
- `error`: an optional engine diagnostic if processing stopped;
- `exception`: a captured worker exception, if one was thrown.

`succeeded() noexcept` means that worker drained the batch without either
failure. The runner sets at most one failure field. Results and committed prefixes
remain owned after later runs or owner destruction. Keep the entire returned
vector until every committed batch has been consumed downstream; inspecting one
worker's failure must not discard another worker's already committed results.

For a fixed input, starting state and assignment, this publication is reproducible
regardless of OS scheduling. A different partition count can change the combined
stream while preserving each instrument's ordered events and final state. There
is no globally increasing timestamp guarantee or portfolio-wide transaction.

## Failure boundaries and retry

Before launching threads, the coordinator reserves one output slot per pending
arrival in every partition, plus all worker/thread container storage. No worker
can commit until every requested thread has been created and a common start
latch is released. If preparation or thread creation throws, existing threads
leave through the unopened gate and join; all books, clocks, counters and pending
arrivals remain unchanged. The coordinator exception propagates to the caller.

After the gate opens, each worker processes independently. An engine diagnostic
or exception stops only that worker and is captured in its `WorkerBatch`.
Its failing queue head remains pending and all preceding committed steps remain
in `steps`; other workers continue. After a commit, moving a step into its
preallocated publication slot cannot allocate or throw. All writers join before
any result or state is exposed.

A later `run()` retries remaining arrivals, without replaying committed ones.
Consume/save the earlier committed prefixes as well as the retry results.
Downstream recording/accounting failures do not undo matching; retain the
returned batches to retry those downstream operations.

An empty `run()` allocates nothing and launches no threads. After drain, new
current/future requests can form another batch with existing instrument IDs,
clock/counter state and worker insertion sequences. Thread join is synchronization
only; modeled time always comes from request timestamps.

## Build and validation

CMake links `Threads::Threads` through the `marketlab` interface target.
The existing `partitioned_market` CLI now accepts `--threaded`; its default stays
caller-driven. Build and compare both modes with the
[README commands](../README.md#run-the-partitioned-scenario). Threaded reports
add `execution=threaded`; event export contains only v1 records, grouped by worker
index. The finite example exits1 if any worker fails and publishes no partial
summary; the library recovery contract above remains available to callers.

The threaded component also has dedicated tests:

```powershell
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --target threaded_simulation_tests threaded_simulation_allocation_tests
ctest --test-dir build/debug --output-on-failure -R "^threaded_simulation(_allocation)?$"
```

The equivalence suite processes 28,800 generated arrivals with 1/24 instruments,
1/4/7 configured workers, two seeds and two consecutive batches. It checks exact
publication, per-instrument events, complete bounded book/market state,
positions/cash and FIFO PnL against the caller-driven runner. Additional tests
replay the phase-10 reference experiment beside another instrument with one or
two actual worker threads, checking its events, depth, spread, fills and accounts.

A separate backlog regression schedules 4,096 bids and matching sells on one
partition and 64 pairs on another, repeated in two batches (16,640 arrivals).
All arrivals in each batch have the same timestamp; descending bid IDs make
FIFO priority observable independently of ID order. Each step is compared with
an isolated exchange. It checks worker-index publication, exact fill terms,
sequence continuity, complete drain, inactive partitions and owned results from
the first run remaining valid after the second. This exercises closed queues;
it does not define a capacity limit or live-ingress backpressure.

Allocation injection distinguishes coordinator preparation/launch from
thread-local worker allocations. It verifies unchanged state on failed startup,
retained committed prefixes and pending heads after worker failures, independent
worker completion, identical prefix-plus-retry results, source ownership and idle
paths. Dedicated lifetime checks consume and record successful executions, apply
them to participant ledgers, and rethrow captured worker allocation exceptions
after the runner and its source configuration have been destroyed. These tests
are correctness checks, not benchmarks or race-detector runs.

The Windows MinGW toolchain has no ThreadSanitizer runtime, but the
concurrency suites now pass under Linux GCC 16.2 with ThreadSanitizer through WSL.
Run `sh scripts/test-thread-sanitizer.sh` using the
[README instructions](../README.md#linux-threadsanitizer-tests) to reproduce the
check. The startup decision uses explicit atomic release/acquire publication in
addition to the latch, and no sanitizer suppressions are used. This validates the
closed-batch workloads exercised by the suites. The standalone
[bounded ingress queue](bounded-ingress.md) is tested separately and is not
connected to this closed-batch runner. The separate
[streaming owner](streaming-simulation.md) supports live routed ingress and
external consumers; this API retains its closed-batch lifecycle. The two dedicated threaded suites
also pass Linux AddressSanitizer and UndefinedBehaviorSanitizer checks, including
leak detection and result/exception access after owner destruction.
