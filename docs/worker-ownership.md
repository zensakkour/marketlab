# Instrument-to-worker assignment

Phase 12 starts with fixed configuration in
`<marketlab/worker_assignment_plan.hpp>`. `WorkerAssignmentPlan` records which
worker index owns each registered instrument. The plan itself creates no threads
or queues.
The [caller-driven partitioned simulation](partitioned-simulation.md) uses it to
own separate exchanges and arrival queues.

## Construction and validation

`WorkerAssignmentPlan(registry, worker_count, assignments)` copies a span of
`InstrumentWorkerAssignment{instrument_id, worker_index}` records. Construction
requires:

- a strictly positive worker count;
- exactly one assignment for every instrument in the registry;
- registered instrument IDs, without duplicate assignments;
- zero-based worker indexes smaller than the worker count.

Identical duplicate assignments are also invalid. There is no automatic placement
or default owner for missing instruments. Workers may be idle, and multiple
instruments may share a worker. An empty registry accepts an empty assignment
list with a positive worker count. Invalid configuration throws
`std::invalid_argument`.

Storage grows with the number of instruments, not the worker count. The count
is configuration metadata, not a promise that the runtime can create that many
threads. Allocation failures propagate without publishing a partial plan or
changing the registry or caller records; construction can be retried.

## Read-only access and lifetime

- `worker_count() noexcept` returns the configured worker count.
- `worker_for(id) noexcept` returns `optional<size_t>`: a present zero means
  worker 0; an absent ID (including zero) returns `nullopt`.
- `assignments() noexcept` borrows a const span sorted by increasing instrument
  ID, independent of the caller's input order.

All queries allocate nothing. The plan owns its records and retains no reference
to the registry or source span. Both inputs may be destroyed after construction,
and later changes to the source records do not affect the plan. The plan cannot be copied, moved or assigned, keeping borrowed
spans valid until its destruction. A new plan constructed from the span owns a
separate copy.

For a registry containing instruments 1 and 2:

```cpp
#include <marketlab/worker_assignment_plan.hpp>

#include <array>

const std::array bindings{
    marketlab::InstrumentWorkerAssignment{{2}, 1},
    marketlab::InstrumentWorkerAssignment{{1}, 0}};
marketlab::WorkerAssignmentPlan plan{registry, 3, bindings};
const auto owner = plan.worker_for({1}); // Present, value 0; worker 2 is idle.
```

## Execution boundary

This immutable plan is the configuration prerequisite for runtime ownership.
The caller-driven runner now provides private worker queues and routed step
results, with one-versus-many-partition correctness comparisons. The plan itself
does not enforce thread access, route requests or advance time. Concurrent
ingress and race detection remain later Phase 12 work. The separate
[threaded closed-batch runner](threaded-simulation.md) now provides exclusive
worker threads, a common startup gate and join-before-publication.
See [current single-writer routing](multi-instrument-exchange.md).

The `worker_assignment_plan` test covers complete mappings, input permutations,
independent ownership, invalid/missing/duplicate bindings, idle workers, empty
registries, maximum IDs/indexes, allocation-failure retry and non-allocating
queries. See [build/test commands](../README.md#debug-build-and-tests).
