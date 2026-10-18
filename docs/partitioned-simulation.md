# Caller-driven partitioned simulation

`PartitionedSimulation` in `<marketlab/partitioned_simulation.hpp>` owns one
`MultiInstrumentExchange` and one `SimulationQueue` per active worker index.
Every instrument belongs to exactly one partition under a copied
[assignment plan](worker-ownership.md). All API calls, including reads and calls
for different workers, must be serialized by one logical writer. This version
creates no threads and provides no concurrent ingress or concurrent readers.
The separate [threaded batch owner](threaded-simulation.md) reuses the same private
worker processing with exclusive thread ownership and joined result publication.

## Configuration and ownership

Construct with `PartitionedSimulation(registry, plan, participants)`. The owner
revalidates the copied assignment list against the supplied registry and copies
instrument metadata and fixed participant registrations. A plan with different
instrument membership is refused. Nonzero participants are required even for an
empty registry; duplicate registration follows existing exchange set semantics.
An empty participant set is allowed and produces normal exchange rejections.

Inputs can be destroyed after construction. The owner cannot be copied, moved
or assigned. Only active workers allocate exchange/queue storage; idle worker
indexes remain valid. Constructor allocation failures publish no partial owner
and leave registry, plan and caller inputs unchanged.

- `plan() noexcept` borrows the immutable copied plan.
- `find(id) noexcept` borrows a const exchange, or returns nullptr for unknown IDs.
- `queue(worker) noexcept` borrows a const queue for an active worker, or returns
  nullptr for an idle or out-of-range index.

These reads allocate nothing. Exchange/queue object addresses stay valid until
owner destruction. A queue's borrowed head must be reacquired after successful
scheduling or removal, following `SimulationQueue`'s existing lifetime rules.
No mutable exchange or queue is exposed.

## Schedule and advance

`schedule(SimulationRequest)` routes by raw instrument ID and returns
`expected<SimulationSequence, PartitionedScheduleError>`:

| Diagnostic | Meaning |
|---|---|
| UnknownInstrument | No destination exists, including ID zero |
| InvalidTimestamp | Submitted time precedes that instrument's committed clock |
| SequenceExhausted | Destination queue cannot assign another insertion sequence |
| CapacityExceeded | Destination queue cannot store another arrival |

Destination lookup precedes the time check. Equal-time scheduling is allowed.
Other malformed order/cancel fields remain unchanged for exchange validation.
Refusal or allocation failure consumes no sequence and changes no queue, book,
clock or event counter. There is no separate scheduling validation against a
worker-wide clock, future pending times or another instrument's clock.

Each worker queue orders by request timestamp, then its own successful insertion
sequence. Arrival identity is `(worker_index, SimulationSequence)`; different
workers can both assign sequence 1. There is no global scheduling counter.

`step(worker)` returns
`expected<optional<WorkerStep>, PartitionedStepError>`. A present step owns its
worker index, removed `SimulationEvent` and complete ordered exchange-event
vector. An empty optional means that worker is drained or idle. An index outside
`[0, plan().worker_count())` returns `InvalidWorker` without mutation.

A worker processes only its own queue head. Order/cancel rejection is successful
committed processing: its event is returned and its arrival consumed. Engine
sequence/capacity diagnostics map to the corresponding `PartitionedStepError`;
they retain the pending head. Allocation exceptions also retain the head and all
committed state, including every other worker's queue/book. Retrying the same step
processes the same arrival. After exchange commit, queue removal and result
transfer cannot allocate or throw.

## Time, publication and lifecycle

Every instrument starts at time zero and retains its existing local clock,
request/event counters, order-ID history, market data and validation semantics.
For example, after processing A at t100, B can still accept t2 if B's committed
clock is at most t2, even when A and B share a worker. A queue's timestamps order
pending arrivals; they do not create a worker-wide exchange clock.

There is no automatic merge across workers. The caller chooses which worker to
advance, and publishes returned batches in that call order. A different partition
or step interleaving can change the combined stream while preserving each
instrument's ordered events and final state. Event identity remains
`(InstrumentId, EventSequence)`. Do not sort merged output by local counters or
assume that successive worker results have globally increasing timestamps.

Empty steps do not advance time. After drain, scheduling current/future arrivals
can resume processing with existing IDs and sequences. Construction and scope
lifetime manage this caller-driven owner. The separate threaded batch owner
provides common-gate startup and join-before-publication; live close/drain
channels and backpressure remain later Phase 12 work. The closed-batch runner
also has Linux ThreadSanitizer validation; see its documentation for scope.

Returned results own their events after subsequent steps or owner destruction.
Accounting and recording remain downstream caller responsibilities. A downstream
allocation/I/O error after receiving a committed step does not undo exchange
processing; retain the returned batch to retry that downstream operation. There
is no whole-experiment rollback guarantee.

## Example and validation

`partitioned_market [worker-count:1..4 [--threaded] [--events]]` defaults to two
caller-driven workers. It schedules a fixed three-instrument script, counts one
unknown destination, then advances workers in ascending round-robin order.
With `--threaded`, the same scenario runs through `ThreadedSimulation` and
publishes joined batches by worker index. No threads are launched in the default
mode. Only nonempty partitions launch threads in threaded mode; the fourth worker
is idle in either case. Flags may appear in either order after the worker count;
duplicate or unknown flags are refused.
Exit codes are 0 for success, 1 for run/output failure and 2 for invalid arguments.

| Result | ALPHA | BETA | GAMMA |
|---|---:|---:|---:|
| Final time (ns) | 12 | 4 | 6 |
| Requests / events | 4 / 9 | 4 / 8 | 3 / 8 |
| Executed volume | 3 | 5 | 3 |
| Explicit mark (ticks) | 101 | 201 | 301 |
| Participant 1 position / cash | +3 / -300 | -5 / +1000 | -3 / +900 |
| Participant 2 position / cash | -3 / +300 | +5 / -1000 | +3 / -900 |
| Participant 1 / 2 total PnL | +3 / -3 | -5 / +5 | -3 / +3 |

There are 12 scheduling attempts, 11 queued arrivals, 1 refused destination and
25 events; all books finish empty. Cash/PnL remain exact in each instrument's own
tick-times-quantity units, with no cross-instrument portfolio sum.

Threaded summary output adds `execution=threaded` before the existing fields;
market/account lines remain identical. Event output contains only v1 records.
LF-normalized v1 event FNV-1a-64 checksums reflect the two publication conventions:

| Workers | Caller-driven round-robin | Threaded worker-index batches |
|---|---|---|
| 1 | `c18fc1aae8162e95` | `c18fc1aae8162e95` |
| 2 | `be71fb86b4a9b923` | `ae1a327fb209f545` |
| 3 or 4 | `dc9eabc282adc655` | `8a9feb10d6d8e7f5` |

Tests retain the literal caller-driven report/checksums, compare both modes over
ten fresh runs per partition count, and independently derive threaded export by
stably grouping the caller-driven events by worker. Every instrument retains its
ordered events and accounts. The finite CLI aborts with exit code1 if a worker
fails; recoverable library callers instead retain the full `WorkerBatch` results.
[README commands](../README.md#run-the-partitioned-scenario) cover builds, runs,
exports and tests. Tests compare complete per-instrument events, bounded-fixture
book/market state, quantity/cash accounts and FIFO PnL with isolated exchanges;
exercise skewed many-symbol workloads and different partition/call orders; and
inject constructor, scheduling and step allocation failures with identical retry.
These are correctness checks, not throughput measurements or concurrency tests.
