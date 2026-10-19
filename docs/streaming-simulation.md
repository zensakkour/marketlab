# Streaming simulation with external consumers

`StreamingSimulation` in `<marketlab/streaming_simulation.hpp>` owns fixed
instrument partitions, their exchanges and a [bounded ingress queue](bounded-ingress.md)
per active worker. It owns no threads or internal result history.

## Ownership and roles

Construct with `StreamingSimulation(registry, plan, participants, queue_capacity)`.
Configuration is copied and revalidated. Capacity must be positive, including for
an empty registry. Storage scales with assigned instruments, not idle indexes.
The owner cannot be copied or moved.

One serialized producer calls `try_schedule(request)` and `close()`. One exclusive
consumer calls `step(index)` per partition. Different consumers may step while
the producer schedules. Never overlap consumers on the same index; synchronize
any transfer to another thread externally. The caller owns startup, exception
handling, downstream delivery and joining. There is no thread-ID enforcement,
pool, callback or automatic stop policy.

`plan()` is immutable. `queue(index)` returns a stable const pointer for internally
synchronized `size()`/`is_closed()` and immutable `capacity()` observations;
idle/invalid indexes return null. Queue size excludes an in-flight arrival.
`find(id)` returns a stable const exchange pointer, or null for an unknown ID.
Obtain/read exchange views after all consumers finish/join. Finish producer and
observer calls before destruction too. Destruction releases storage; it does not
close, drain or join external threads.

## Scheduling

`try_schedule(SimulationRequest)` returns
`expected<SimulationSequence, StreamingScheduleError>`. It routes through the
immutable assignment without reading mutable books or exchange clocks.
`UnknownInstrument` is checked first, including after close. A known destination
then follows the queue's `Closed`, `InvalidTimestamp`, `SequenceExhausted`, `Full`
diagnostic precedence.

Only success copies the request and assigns the next positive queue-local
sequence. Refusals preserve sequence, time watermark and pending arrivals.
Raw business fields remain for exchange validation. Times must be nondecreasing
**per partition**, across all its instruments; equal times retain producer order.
Independent partitions can have different times. This FIFO does not sort future
requests or validate against an individual instrument's committed clock.

Prepare ordered input and retry the same `Full` request before advancing the
producer stream. Full/empty poll counts may depend on OS scheduling; accepted
inputs, per-partition sequences and per-instrument outcomes do not under that
rule. There is no implicit global publication order or portfolio transaction.

## Step and failure recovery

`step(index)` returns `expected<WorkerStep, StreamingStepError>`. Success owns the
worker index, original arrival and complete exchange-event vector, including
committed order/cancel rejections. Results survive later calls and destruction.

Without a retained arrival, a step pops one request from ingress. The worker
keeps it until exchange processing succeeds. An engine diagnostic or allocation
exception preserves exchange state and this in-flight request. A later step
retries it before popping a successor, including after close. After commit,
clearing the retained arrival and transferring the result cannot allocate/throw.

Step diagnostics are `InvalidWorker`, `Empty` (open and no work), `Closed`
(closed and both ingress/in-flight storage drained), or the retained-request
engine errors `RequestSequenceExhausted`, `EventSequenceExhausted` and
`OutputCapacityExceeded`. Valid idle indexes return `Empty` before global close
and `Closed` afterward. Allocation exceptions propagate to that consumer.

Other workers remain independent. The caller catches failures and decides
whether to retry or stop its producer/consumers. A stopped consumer does not
automatically close ingress. Already returned results are committed; keep them
for downstream recording/accounting retry.

Each worker has at most `queue_capacity` queued requests plus **one in-flight
request**. A producer may fill the freed slot while processing/retry is pending.
This bound covers accepted unprocessed requests, not books, lifetime ID history
or caller-owned output/history storage.

## Close and drain

`close()` is serialized with scheduling. It closes active queues, then publishes
`is_closed() == true`; repeated successful calls do nothing. Closure is not an
atomic snapshot across partitions: one consumer may observe closed ingress while
another partition is still being closed. The final flag reports input closure,
not completion of matching.

Accepted queued/in-flight requests remain available. Consumers step until
`Closed`, handle exchange failures explicitly, then the caller joins them.
There is no reopening. If mutex acquisition fails during close, retry to finish
closing remaining queues; already closed queues remain closed. Tests do not
inject OS mutex failures.

## Build and validation

```powershell
cmake --build build/debug --target streaming_simulation_tests streaming_simulation_allocation_tests
ctest --test-dir build/debug --output-on-failure -R "^streaming_simulation(_allocation)?$"
```

The behavior suite compares 43,200 requests on actual external consumer threads
with the original heap-driven runner: two seeds, 1/12 instruments, 1/4/7 workers
and capacities 1/7/64. It checks exact events, FIFO sequences, books, market state,
cash/positions and FIFO PnL. Boundary cases cover copied sources, owned results,
raw invalid requests, idle/sparse maximum indexes, partition time and close/drain.
Two seeded reference market-making runs also pass through capacity-one ingress
on an actual consumer thread. Their delivered inputs exclude the original
500-nanosecond boundary; events, replayed spread, fills and participant accounts
match the original experiment. The streaming owner does not impose a horizon.

Allocation injection covers construction and each allocation in passive/crossing
limits, markets, cancels and rejections. It verifies automatic pending retention,
the capacity-plus-one bound, independent worker completion, closed retry/drain,
exact results and nonallocating ingress/read/empty/refusal paths. Exhaustion
mapping has compile-time checks; tests do not perform `2^64` requests. The
[README](../README.md#linux-threadsanitizer-tests) includes both suites in Linux
ThreadSanitizer and separate ASan/UBSan commands.
