# Caller-driven partitioned simulation

`PartitionedSimulation` in `<marketlab/partitioned_simulation.hpp>` owns one
`MultiInstrumentExchange` and one `SimulationQueue` per active worker index.
Every instrument belongs to exactly one partition under a copied
[assignment plan](worker-ownership.md). All API calls, including reads and calls
for different workers, must be serialized by one logical writer. This version
creates no threads and provides no concurrent ingress or concurrent readers.

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
lifetime manage this caller-driven owner; threaded startup, close/drain channels,
shutdown/join, backpressure and race detection remain later Phase 12 work.

Returned results own their events after subsequent steps or owner destruction.
Accounting and recording remain downstream caller responsibilities. A downstream
allocation/I/O error after receiving a committed step does not undo exchange
processing; retain the returned batch to retry that downstream operation. There
is no whole-experiment rollback guarantee.

## Example and validation

`partitioned_market [worker-count:1..4 [--events]]` defaults to two workers.
It schedules a fixed three-instrument script, counts one unknown destination,
then advances workers in ascending round-robin order. The count is the number
of logical partitions. No worker threads are launched. The fourth worker is idle.
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

LF-normalized v1 event FNV-1a-64 checksums are `c18fc1aae8162e95` (one worker),
`be71fb86b4a9b923` (two), and `dc9eabc282adc655` (three or four).
[README commands](../README.md#run-the-partitioned-scenario) cover builds, runs,
exports and tests. Tests compare complete per-instrument events, bounded-fixture
book/market state, quantity/cash accounts and FIFO PnL with isolated exchanges;
exercise skewed many-symbol workloads and different partition/call orders; and
inject constructor, scheduling and step allocation failures with identical retry.
These are correctness checks, not throughput measurements or concurrency tests.
