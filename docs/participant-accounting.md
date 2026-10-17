# Participant accounting

Phase 8 quantity and cash-flow accounting is implemented in
`<marketlab/participant_ledger.hpp>`: execution history, bought/sold volume, net
positions, exact spent/received/net cash and total PnL at explicit prices for one
configured instrument. `<marketlab/participant_pnl.hpp>` adds FIFO realised and
unrealised PnL reconstructed on demand from its history.

## Ownership and initial state

`ParticipantLedger(InstrumentId, std::span<const ParticipantId>)` owns accounting
separately from the exchange. It copies a fixed participant set and starts with
zero bought/sold volumes, zero positions, zero spent/received cash and an empty
history. Zero instrument or participant IDs throw `std::invalid_argument`; repeated registrations name the
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

## Participant fill counts

`participant_fills(ledger, participant) noexcept` from
`<marketlab/participant_fills.hpp>` returns an owned optional
`ParticipantFills`. Unknown participants, including zero, return an empty
optional; registered participants with no executions have four zero counters.

A **fill** here is one counterparty leg of one execution, regardless of quantity
or whether the order is fully filled. `buy_count` and `sell_count` count the
participant's buying/selling legs. `aggressive_count` and `passive_count` count
its incoming/resting legs. Across all participants each execution contributes
one buy, one sell, one aggressive and one passive fill. A self-match contributes
one to all four counters of the same participant, consistently with gross
accounting, while its net inventory and cash remain unchanged.

A market order hitting two resting orders therefore has two aggressive fills.
A resting order partially executed three times has three passive fills.
Acceptance, cancellation, rejection and unfilled expiry add no fill. A single
execution of INT64_MAX units still adds only one per relevant leg.
These counts are distinct from `OrderFilled` status events and traded volume.

The projection scans the ledger's committed execution history in O(history)
time and O(1) extra storage, without allocation, cached mutable counters or state
changes. Returned values survive later ingestion and source destruction.
Each individual counter increases at most once per execution, so the ledger's
strict positive uint64 event ordering bounds it by UINT64_MAX. Directional/role
counters remain separate; the API does not narrow the sum of two counters.

The controlled-market CLI exposes the counts with
[`--fills`](../README.md#inspect-controlled-participant-fills).

## Cash flows and exact notionals

Accounts also store `buy_notional` and `sell_notional` as exact nonnegative
`CashAmount` values. An execution contributes `price.ticks() * quantity.units()`
to the buyer's spent notional and the seller's received notional. Unfilled,
cancelled and expired quantities contribute nothing.

`ParticipantAccount::cash_flow() noexcept` returns `NetCashFlow{negative, magnitude}`:
received notional minus spent notional, starting from zero. A buy is a debit
(negative), a sale is a credit, and equal totals produce canonical nonnegative
zero. A self-match adds equal spent/received notionals without changing net cash.
Net cash may be negative. This ledger tracks execution-driven flows; it does not
model funded balances, credit limits, solvency checks, fees or cash reservations.

The unit is **one price tick times one quantity unit**, for the ledger's configured
instrument. To interpret the signed value monetarily, multiply by that instrument's
`TickSize` coefficient and by 10 to the negative scale. For example, buying 2 units
at 101 ticks produces -202 tick-times-quantity units; with tick size 0.01, this is
-2.02 in quote-value units. No rounding or floating point enters stored cash.
A monetary formatter and aggregation across instruments/currencies are separate
work; do not treat raw tick notionals from different instruments as one cash unit.

`CashAmount` from `<marketlab/cash_amount.hpp>` uses three unsigned 64-bit words:
`high_units * 2^128 + middle_units * 2^64 + low_units`. It is distinct from
`AggregateQuantity`; cash and position units cannot be implicitly interchanged.
`from_trade(Price, Quantity)` computes an exact trade product using portable
32-bit partial products. A zero quantity produces zero.

`from_aggregate(Price, AggregateQuantity)` computes the corresponding exact
notional for a cumulative quantity, including sizes above UINT64_MAX. Every
unsigned 128-bit quantity times a positive int64 price is below 2^191, so this
returns a `CashAmount` directly without an overflow case. It is constexpr/noexcept
and allocates nothing. This is an arithmetic conversion: callers supply the
price; the helper does not choose a market mark or implement a PnL convention.

Each valid trade has price and quantity at most INT64_MAX, so its notional is
less than 2^126. At most UINT64_MAX distinct increasing event sequences can enter
one ledger, so each gross cash total is less than 2^190. Three words therefore
represent every valid ledger total, including values beyond 128 bits. The net
flow's magnitude is at most the larger gross total and fits the same bound.

For standalone caller-created values, `CashAmount::add()` reports overflow with
an empty optional; `subtract()` reports a negative result likewise. Both preserve
the source, propagate carries/borrows across all three words and are
constexpr/noexcept without allocation. Comparisons are exact. The decimal
`marketlab::to_string(CashAmount)` handles the complete unsigned 192-bit range;
its returned string may allocate.

`CashAmount::multiply(std::uint64_t factor)` scales an amount exactly by a
nonnegative integer. It returns an empty optional if the product exceeds
UINT192_MAX, including overflow from any upper partial product or carry. Zero
and one factors are supported. The operation preserves the source and is
constexpr/noexcept without allocation; it does not select a valuation price or
change ledger state.

Cash is prepared and committed in the same batch as quantities and history.
Validation/allocation failure preserves every gross/net value. Retrying retained
executions adds their cash once, and replay reconstructs quantities and cash
together. Execution price, rather than an incoming limit or a later market quote,
determines the notional.

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
`fresh.apply(original.history())` to reconstruct the quantity and cash-flow accounts exactly.
Whole-history and incremental replay produce the same gross volumes, net
positions, cash notionals/flows and complete execution records. This is in-memory
reconstruction; event-log parsing and persistent replay tools remain later roadmap work.

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
    const auto cash = account->cash_flow();
    const auto tick_units = marketlab::to_string(cash.magnitude);
    // cash.negative distinguishes a net debit from credit/zero.
    const auto mark = marketlab::Price::from_ticks(102).value();
    const auto pnl = account->total_pnl(mark).value();
    // pnl is read-only total PnL at this explicit price, in tick-times-quantity units.
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

At 100 ticks for each trade in this example, A's net cash flow is -500 after
the first trade and -300 after the reversal; the self-match leaves it at -300.
B has the opposite net flow.

`participant_ledger` tests configuration, snapshots, gross/net reconciliation,
self-matches, ordering/refusal precedence, invalid suffix rollback, correction,
history replay, quantity totals above UINT64_MAX, cash totals above 128 bits and
maximum sequence/time values.
`participant_ledger_allocation` injects each preparation/history allocation,
checks unchanged borrowed views and retry, and verifies non-allocating reads,
empty batches and refusals, including non-allocating cash reads. It also retries
quantity/cash accounting from a retained multi-level simulation result while checking that committed exchange state and
the next pending arrival stay unchanged.

Generated exchange workloads compare every account's gross volumes and signed
net position, spent/received notional and net cash with independent decimal
arithmetic after every request, then rebuild and compare complete history. Total
debits and credits must reconcile across the configured participants. Twenty fresh
scripted replays check accounting alongside exact events, final book and market observations, against
a run without accounting ingestion.

`unsigned_product` checks all 4,096 single-bit operand pairs, exact integer
boundaries and 10,000 generated products against independent decimal schoolbook
multiplication. `cash_amount` checks carry/borrow/overflow, decimal boundaries
through UINT192_MAX, five maximum trade products and 10,000 independent decimal
product/sum comparisons. Another 10,000 generated scalar products compare
fitting results and overflow decisions to full-width decimal multiplication.
An additional 10,000 full-width aggregate-quantity/price products are checked
against decimal arithmetic, alongside bounded-trade agreement, maximum input
boundaries and a real ledger position above UINT64_MAX.

The [README](../README.md#inspect-the-scripted-participant-accounts) gives build,
test and `--account-script` commands, including signed `cash_tick_units` output.
Funding and fees remain outside the implemented accounting scope.

## Total PnL at an explicit mark

`ParticipantAccount::total_pnl(Price mark) noexcept` returns an optional
`TotalPnl{negative, magnitude}` in the same tick-times-quantity units as cash:

```text
total PnL = net cash flow + signed net position * mark.ticks()
```

The caller supplies a positive whole-tick price for the ledger's instrument.
The account does not choose a last trade, bid, ask, midpoint or stale-price
fallback. An explicit mark is required even for a flat account; its result is
then independent of the mark. This read-only query leaves cash, volumes, history
and exchange counters unchanged. Revalue by calling it again with a new price.
A self-match leaves the result unchanged at a fixed mark. Zero is canonical
nonnegative; `TotalPnl` is an alias for `PnlAmount`, distinct from `NetCashFlow`.

The result includes the effect of closed trades and the marked open position.
It does not split realised and unrealised PnL or assign a cost basis. Starting
cash/inventory are zero and fees are absent, as in the existing cash-flow ledger.
The mark carries no instrument ID or timestamp: the caller is responsible for
its instrument and observation time. Half-tick marks are outside this API.

Ledger cash magnitude is below 2^190, and its position magnitude below 2^127.
Multiplying by a positive int64 mark stays below 2^190, so the total magnitude
is below 2^191 and fits `CashAmount`. Every ledger-produced account therefore
returns a value. Because callers can also construct arbitrary `ParticipantAccount`
values, same-sign addition may exceed 192 bits; that case returns an empty
optional, without wrapping or saturation. Opposite signs use exact subtraction.
The calculation is constexpr/noexcept and allocates nothing.

Examples, starting at zero:

| Executions for A | Net cash | Net position | Mark in ticks | Total PnL |
| --- | ---: | ---: | ---: | ---: |
| Buy 5 at 100 | -500 | +5 | 104 | +20 |
| Sell 5 at 100 | +500 | -5 | 104 | -20 |
| Buy 5 at 100, sell 5 at 104 | +20 | 0 | 110 | +20 |
| Buy 5 at 100, sell 7 at 104 | +228 | -2 | 103 | +22 |

Counterparties valued at the same mark have offsetting total PnL. Rebuilding
an account from the same executions produces the same result at any given mark.
Tests cover partial closes, reversals, self-matches, full-range marks, large
positions/cash, signed cancellation, caller-created overflow and non-allocating
reads. Generated workloads compare to independent decimal credits minus debits
at marks 1, 103 and INT64_MAX after every request and reconcile gains with losses.

The [README](../README.md#inspect-total-pnl-at-a-chosen-price) documents the
`--pnl-script <mark_ticks>` example. FIFO decomposition uses the same explicit
mark and preserves this total.

## FIFO realised/unrealised decomposition

`fifo_pnl(const ParticipantLedger&, ParticipantId, Price mark)` from
`<marketlab/participant_pnl.hpp>` returns an owned optional `PnlBreakdown` with
`realised`, `unrealised` and `total` `PnlAmount` fields. Unknown participants,
including zero, return an empty optional. A registered participant starts at zero.
The mark follows the same explicit instrument/whole-tick rules as total PnL.

The query replays committed executions in their recorded order. Each non-self
execution first closes opposite-side lots from oldest to newest; a partial close
retains the lot's entry price and FIFO position. Any remaining quantity opens a
new lot at the execution price. A reversal can close the old position and open
the opposite position in the same execution.

For a closed long, realised PnL is `(exit - entry) * closed_quantity`; for a closed
short it is `(entry - exit) * closed_quantity`. A self-match retains its existing
gross volumes/cash/history effects but does not open or close cost-basis lots and
leaves realised PnL unchanged. All arithmetic uses exact tick-times-quantity units,
with no fees, funding, floating point, division or rounding.

Unrealised PnL is computed directly from the remaining lots: longs contribute
`(mark - entry) * remaining`, shorts `(entry - mark) * remaining`. Total PnL
comes from the account's cash/position identity, independently of FIFO. Thus:

```text
realised PnL + unrealised PnL = total PnL
```

Examples, each starting from zero:

| Executions for A | Mark | Realised | Unrealised | Total |
| --- | ---: | ---: | ---: | ---: |
| Buy 3 at 100, buy 2 at 110, sell 4 at 120 | 115 | +70 | +5 | +75 |
| Sell 3 at 110, sell 2 at 100, buy 4 at 90 | 95 | +70 | +5 | +75 |
| Buy 2 at 100, sell 3 at 120 | 115 | +40 | +5 | +45 |
| Buy 2 at 100, sell 2 at 90 | 1000 | -20 | 0 | -20 |

The first row closes all three 100-tick units and one 110-tick unit. Its remaining
unit keeps entry price 110. This distinguishes FIFO from average-cost or LIFO
accounting. Revaluing at 105 changes unrealised PnL to -5 and total to +65;
realised PnL stays +70.

Lots are temporary query state, stored in a deque; ingestion keeps no cached
cost basis or separately mutable realised balance. One query costs O(history
length) time and O(peak open lot count) temporary storage for the participant.
It may allocate and is not `noexcept`. Allocation exceptions propagate while
preserving accounts, history, sequence counters and borrowed views. Retry the
query, without replaying execution ingestion. Unknown participants and an empty
history return without allocating. Returned values survive later ingestion or
ledger destruction. There is no concurrent mutation support.

Closed credit/debit totals are subsets of gross executed sell/buy notionals.
Open entry and marked notionals are also below 2^190 under the ledger's existing
quantity/event bounds. Each component is therefore representable in 192 bits;
the query never rounds or saturates valid history. `PnlAmount::from_difference()`
derives canonical sign/magnitude from exact nonnegative credits and debits.
`TotalPnl` is retained as an alias for source compatibility.

Only total PnL must sum to zero across participants at a common mark: individual
realised and unrealised sums need not vanish. For example, A buying 1 from B at
100 then selling 1 to C at 110 realises +10 while B's open short has -10 unrealised
at mark 110; C has zero PnL at that mark.

`participant_pnl` tests FIFO selection, partial/full closes, reversals in both
directions, long/short losses, self-matches, retained projections, reconstruction,
full-width quantities/prices and final event sequences. A separate oracle expands
1,000 generated executions into individual units and checks all three PnL values
after each execution at marks 1, 99 and INT64_MAX using decimal arithmetic.
Allocation injection exercises temporary deque creation/growth and exact query
retry, including projection from retained committed simulation executions.

The [README](../README.md#inspect-fifo-realised-and-unrealised-pnl) gives the
`--fifo-pnl-script <mark_ticks>` command. FIFO is the implemented convention;
alternative cost-basis methods, funding, fees and half-tick marks remain outside
this API.
