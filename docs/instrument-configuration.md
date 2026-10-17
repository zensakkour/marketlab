# Instrument configuration

`InstrumentRegistry` from `<marketlab/instrument_registry.hpp>` owns a fixed
set of instrument configurations. The
[multi-instrument exchange](multi-instrument-exchange.md) copies these
configurations into independent exchanges under one logical writer.

## Construction and identity

Construct from `std::span<const Instrument>`, or use the default empty registry.
Every entry must satisfy the existing `is_valid(Instrument)` contract: nonzero
ID and nonempty symbol, with a valid `TickSize` already enforced by that type.
Configuration is copied, including symbol strings; the caller may modify or
discard its original list afterward.

Instrument IDs must be unique. Repeated IDs throw `std::invalid_argument`,
including identical repeated records; conflicting definitions never overwrite
one another. Invalid records also throw `std::invalid_argument`. Allocation
exceptions propagate. Failed construction leaves caller data unchanged and
publishes no partial registry.

Identity is the numeric `InstrumentId`. Symbols are metadata: different IDs
may use the same symbol. The registry does not resolve names, normalize tick
sizes, convert prices or assign IDs.

```cpp
#include <marketlab/instrument_registry.hpp>

#include <array>

const auto cents = marketlab::TickSize::from_decimal(1, 2).value();
const auto half_unit = marketlab::TickSize::from_decimal(5, 1).value();
const std::array instruments{
    marketlab::Instrument{{2}, "SECOND", half_unit},
    marketlab::Instrument{{1}, "FIRST", cents},
};
marketlab::InstrumentRegistry registry{instruments};
const auto* first = registry.find({1}); // Owned configuration, observed through const access.
```

## Read-only access and lifetime

- `find(InstrumentId) noexcept` returns a borrowed `const Instrument*`, or
  nullptr for an absent ID, including zero.
- `instruments() noexcept` returns a borrowed `span<const Instrument>` in
  strictly increasing ID order, independent of input ordering.
- `size()` and `empty()` are non-allocating/noexcept queries.

Stored records cannot be changed through the API. There is no add/remove/update
operation after construction. The registry cannot be copied, moved or assigned.
Pointers and spans remain valid until registry destruction; further reads do not
invalidate them. To create an independent owned copy of configuration, construct
another registry from the first registry's `instruments()` span.

Construction copies symbol storage and sorts the records. Lookup uses binary
search; listing exposes existing storage without copying. Reads allocate nothing
and create no exchange events or simulation sequences.

## Boundary to exchange state

This registry owns configuration only. It does not own books, registrations of
participants, order-ID history, clocks, event counters, market snapshots or
accounting. It does not change any single-instrument rejection convention.

The routed owner keeps clocks, request/event counters and accepted client IDs
local to each instrument. It exposes read-only exchanges for existing market-data
projections; downstream ledgers remain separate per instrument. The configuration
registry itself still owns none of that mutable state.

The `instrument_registry` test checks retained owned configurations, all input
permutations, read-only stable views, minimum/maximum IDs, exact tick parameters,
shared symbol text, invalid/duplicate definitions, every construction allocation
failure followed by retry, and allocation-disabled empty construction/lookups.
See [README build/test commands](../README.md#debug-build-and-tests).
