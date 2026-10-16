# Participant accounting

Phase 8 quantity accounting is implemented in
`<marketlab/participant_ledger.hpp>`: execution history, bought/sold volume and
net positions for one configured instrument. Cash and PnL remain the next part
of this phase and require exact-unit and valuation conventions.

## Ownership and initial state

`ParticipantLedger(InstrumentId, std::span<const ParticipantId>)` owns accounting
separately from the exchange. It copies a fixed participant set and starts with
zero bought/sold volumes, zero positions and an empty history. Zero instrument or
participant IDs throw `std::invalid_argument`; repeated registrations name the
same participant. An empty set is valid but cannot ingest any execution.
The ledger cannot be copied or moved.

The ledger does not submit orders, change the book, allocate exchange sequences
or own simulation time. Use one logical writer for ingestion and read between
completed calls; the ledger provides no synchronization or callbacks.
[Matching semantics](matching-semantics.md#11-authoritative-state)
remain authoritative. Only committed executions change filled quantities;
submission, acceptance, resting, expiry, rejection and cancellation do not.

## Account values and exact positions

`account(ParticipantId) noexcept` returns an owned optional `ParticipantAccount`.
Unregistered IDs, including zero, return an empty optional. Registered accounts
have exact `AggregateQuantity` values `buy_volume` and `sell_volume`.
A saved account remains unchanged after later ingestion or source destruction;
modifying that copy cannot mutate the ledger.

`ParticipantAccount::position() noexcept` returns `NetPosition`:
`negative` records a short position and `magnitude` is its exact absolute size.
It derives bought volume minus sold volume with checked aggregate subtraction.
Equal totals produce canonical zero with `negative == false`. No signed
64-bit narrowing, floating point or cached position is involved.

Each buyer gains the execution quantity in bought volume; each seller gains it
in sold volume. A permitted self-match adds to both gross volumes and execution
history while leaving net position unchanged. Each execution contributes once
to each side across all participants. With zero initial positions, net quantities
sum to zero, and total bought and sold units both equal exchange traded volume.

Strictly increasing uint64 event sequences bound the number of accepted
executions by UINT64_MAX. Each quantity is at most INT64_MAX, so each account's
gross volume is less than 2^127 and fits the existing two-word aggregate type.
The sign/magnitude difference is exact at the same boundaries.

## Ingest an execution batch

`apply(std::span<const Execution>)` returns
`std::expected<void, AccountingError>`. Success applies the entire span and
appends exact execution copies to history. An empty span succeeds without
allocation or state change.

The source must be committed exchange output. The ledger checks record shape,
configured instrument, registered counterparties and delivery ordering; it
does not reconstruct a book or establish the authenticity of a supplied trade.

Records are checked in input order, using this precedence:

| Error | Meaning |
| --- | --- |
| `InvalidExecution` | Intrinsic execution validation failed, including zero IDs/sequences or nonpositive size. |
| `WrongInstrument` | A valid execution names another instrument. |
| `UnknownParticipant` | Buyer or seller is outside the configured set. |
| `OutOfOrder` | Event sequence is not strictly increasing, time/request sequence moves backward, or executions sharing a request sequence disagree on time. |
| `HistoryCapacityExceeded` | The complete validated batch cannot fit the history vector's size limit. |

Ordering is checked against the last accepted execution and earlier records in
the same batch. Event/request sequence gaps are allowed: many exchange events
are not executions. Multiple fills may share request sequence and time; their
event sequences retain matching order. The maximum uint64 event sequence is
usable; nothing may wrap or apply that sequence again. Ledger
`last_event_sequence()` means the last **execution** sequence, initially zero,
rather than the exchange's last status/rejection sequence.

Validation failure refuses the entire batch, including a valid prefix. There is
no partial acceptance or automatic duplicate skipping. A corrected or retained
batch can be retried without double counting.

Participant updates are prepared in temporary storage. History end-insertion
has the strong exception guarantee for nonthrowing execution value records;
only a nonthrowing account-map swap follows it. Allocation exceptions propagate
and preserve accounts, history, last sequence and existing borrowed history views.
This intentionally simple implementation copies the fixed account map per
nonempty batch; no throughput claim or optimization is implied.

## History, replay and delivery failures

`history() noexcept` returns a borrowed `std::span<const Execution>` in accepted
order. Reacquire it after successful nonempty ingestion. Account queries return
owned values; history queries borrow records. Neither query allocates.

Construct a fresh ledger with the same configuration and call
`fresh.apply(original.history())` to reconstruct the quantity accounts exactly.
Whole-history and incremental replay produce the same gross volumes, net
positions and complete execution records. This is in-memory reconstruction;
event-log parsing and persistent replay tools remain later roadmap work.

Ledger delivery occurs after exchange commitment. If accounting allocation fails,
retain the exchange output and retry accounting with those same execution values.
Do not resubmit the exchange request or redeliver its simulation arrival as an
accounting retry. A successful ledger ingestion followed by an accidental repeat
is refused as out of order.

After obtaining a successful `SimulationStep step`, collect its executions into
a batch for an existing ledger:

```cpp
std::vector<marketlab::Execution> executions;
for (const auto& event : step.events) {
    if (const auto* execution = std::get_if<marketlab::Execution>(&event)) {
        executions.push_back(*execution);
    }
}
const auto applied = ledger.apply(executions);
// Inspect applied.error() on refusal. Retain step/executions if allocation throws.
const auto account = ledger.account(marketlab::ParticipantId{1});
if (account) {
    const auto position = account->position();
    const auto units = marketlab::to_string(position.magnitude);
    // Use position.negative to distinguish short from long/flat.
}
```

Execution collection can itself allocate; retaining the original returned step
also permits retrying collection. No financial state changes during collection.

## Reference scenarios and validation

For participants A and B starting at zero:

| Committed executions | A bought | A sold | A net | B bought | B sold | B net |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| None; passive orders, cancellation or rejection only | 0 | 0 | 0 | 0 | 0 | 0 |
| A buys 5 from B | 5 | 0 | +5 | 0 | 5 | -5 |
| Then B buys 2 from A | 5 | 2 | +3 | 2 | 5 | -3 |
| Then A self-matches 4 | 9 | 6 | +3 | 2 | 5 | -3 |

`participant_ledger` tests configuration, snapshots, gross/net reconciliation,
self-matches, ordering/refusal precedence, invalid suffix rollback, correction,
history replay, totals above UINT64_MAX and maximum sequence/time values.
`participant_ledger_allocation` injects each preparation/history allocation,
checks unchanged borrowed views and retry, and verifies non-allocating reads,
empty batches and refusals. It also retries accounting from a retained
multi-level simulation result while checking that committed exchange state and
the next pending arrival stay unchanged.

Generated exchange workloads compare every account's gross volumes and signed
net position with independent decimal arithmetic after every request, then
rebuild and compare complete history. Twenty fresh scripted replays check
accounting alongside exact events, final book and market observations, against
a run without accounting ingestion.

The [README](../README.md#inspect-the-scripted-participant-accounts) gives build,
test and `--account-script` commands. Cash units, funding, cost basis, realised
PnL, marks and total PnL are not implemented by this quantity ledger.
