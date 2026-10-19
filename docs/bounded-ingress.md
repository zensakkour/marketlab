# Bounded concurrent ingress queue

`BoundedIngressQueue` in `<marketlab/bounded_ingress_queue.hpp>` owns a fixed
capacity FIFO of `SimulationEvent` values. It is the first bounded transport
primitive for phase 12. It is tested independently and is now owned by
[StreamingSimulation](streaming-simulation.md). The separate
[threaded runner](threaded-simulation.md) still accepts only closed batches.

## Ownership and ordering

Construct with a strictly positive capacity. Zero throws `invalid_argument`;
unrepresentable storage or allocation failure prevents construction. The queue
preallocates its slots and never grows that buffer. It cannot be copied or moved.

One serialized producer calls `try_push(request)` and `close()`. One serialized
consumer calls `try_pop()`. The producer and consumer may run concurrently.
The queue uses a mutex; the `try_` operations return immediately on full/empty
state but may wait to acquire that mutex. They are not lock-free operations.

Accepted timestamps must be nondecreasing across the whole queue, including
different instruments and requests added after a drain. Equal timestamps retain
insertion order. The first timestamp may be zero. Unlike `SimulationQueue`, this
FIFO does not sort future requests: the producer supplies delivery order.

Each successful push assigns the next positive `SimulationSequence`, scoped to
this queue. Draining does not reset the sequence or last accepted timestamp.
Every refusal leaves all pending arrivals, the sequence and timestamp unchanged.
Raw order/cancel fields are copied as submitted, including malformed IDs,
quantities and prices. Exchange validation remains the eventual consumer's job.

## API and diagnostics

| Operation | Result |
|---|---|
| `try_push(SimulationRequest)` | `expected<SimulationSequence, IngressPushError>` |
| `try_pop()` | `expected<SimulationEvent, IngressPopError>` |
| `close()` | Idempotent producer-side close; accepted arrivals remain pending |
| `capacity() noexcept` | Immutable configured slot count |
| `size()` | Synchronized pending count at the instant of the call |
| `is_closed()` | Synchronized closed flag, including while draining |

Push errors have this precedence:

1. `Closed`: producer has closed the queue.
2. `InvalidTimestamp`: timestamp precedes the last successfully accepted one.
3. `SequenceExhausted`: the last assigned sequence is `UINT64_MAX`.
4. `Full`: every slot contains a pending arrival; retry after a consumer pop.

Pop returns `Empty` for an open empty queue. After close, queued arrivals still
pop normally in FIFO order; only a closed **and empty** queue returns `Closed`.
The queue never reopens. There is no blocking wait or notification API in this
first primitive. Synchronized observations do not reserve a slot for a later call.

For example, with capacity one:

```cpp
marketlab::BoundedIngressQueue queue{1};
const marketlab::CancelRequest request{{10}, {1}, {2}, {3}};
const auto first = queue.try_push(request);  // sequence 1
const auto full = queue.try_push(request);   // IngressPushError::Full
const auto arrival = queue.try_pop();        // owned sequence-1 request
const auto retry = queue.try_push(request);  // sequence 2
queue.close();
const auto last = queue.try_pop();           // owned sequence-2 request
const auto end = queue.try_pop();            // IngressPopError::Closed
```

## Delivery and lifecycle boundary

A successful pop transfers an owned arrival and frees its slot. The arrival
survives subsequent slot reuse and queue destruction. Once popped, retaining or
retrying that in-flight arrival after an exchange error belongs to the consumer;
this transport alone does not provide exchange-commit acknowledgment or rollback.

For reproducibility, the producer must retain a request refused with `Full` and
retry it before advancing its input stream. The number of full/empty polls may
depend on OS scheduling; the accepted stream and assigned sequences do not when
the producer follows that rule. `Full` is transport backpressure, not an exchange
rejection, and emits no exchange event. This bound covers pending queue storage,
not downstream result/history buffers.

Join/finish every external producer, consumer and observer before destruction.
The queue owns no thread and its destructor does not close, drain or join one.
Mutex acquisition errors propagate before an operation changes state. Copying
and transferring stored requests cannot allocate or throw after the lock is held.

## Build and validation

```powershell
cmake --build build/debug --target bounded_ingress_queue_tests bounded_ingress_queue_allocation_tests bounded_ingress_exchange_tests bounded_ingress_exchange_allocation_tests
ctest --test-dir build/debug --output-on-failure -R "^bounded_ingress_(queue|exchange)(_allocation)?$"
```

Behavior tests cover full refusal/retry, wraparound, persistent timestamps and
sequences, malformed payloads, maximum time/IDs, owned result lifetimes, capacity
boundaries, close/drain and empty-close observation. Concurrent checks send
20,000 mixed requests for each capacity 1, 2, 7 and 64, verifying every payload,
sequence and exact count. Test-side yields coordinate OS execution only; modeled
time comes from request timestamps. Allocation injection checks construction
failure/retry and repeated operations with allocation disabled after construction.
Exhausting a 64-bit sequence through actual pushes is not a practical test.

`bounded_ingress_exchange` composes the existing queue and exchanges with
test-owned producer/consumer threads. Across 43,200 requests it compares exact
per-worker events, complete bounded-fixture book/market state, cash, positions
and FIFO PnL with `PartitionedSimulation`. The reference heap receives the
original unsorted script; transport input is stably sorted by logical time,
preserving insertion order at equal timestamps. This checks the FIFO path
against the existing scheduler without duplicating its heap implementation.
Cases cover two seeds, 1/12 instruments, 1/4/7 workers and capacities 1/7/64.

`bounded_ingress_exchange_allocation` exercises passive/crossing limits, markets,
cancels and rejected orders/cancels. A consumer-owned arrival stays outside the
capacity-one queue while its accepted successor remains inside. After closing
ingress, every allocation point in the first request is failed in turn. The
consumer retries its owned arrival before popping the successor, and exact
events/final state match an uninterrupted exchange. This validates the documented
ownership responsibility with existing APIs. The separate streaming suites test
automatic in-flight retention and routed close/drain through the owning API.

The [README](../README.md#linux-threadsanitizer-tests) includes these suites in
ThreadSanitizer validation and documents separate Linux ASan/UBSan commands.
The [streaming owner](streaming-simulation.md) defines routing, in-flight retry
and external consumer lifecycle without changing this transport API.
