# Reference market-making experiment

Phase 10 provides `market_making [--events] [seed]`, a deterministic reference
experiment using the existing single-writer exchange, controlled plans,
execution ledger and FIFO valuation. The separate three-participant
`controlled_market` example remains available.

## Configuration and ownership

One instrument, ID 1 / `REFERENCE`, has tick size 0.01. Both the fixed reference
and explicit FIFO valuation mark are 100 ticks. The default seed is 42; decimal
seeds from 0 through UINT64_MAX are accepted.

| ID / role | Fixed configuration |
|---|---|
| 1 / background liquidity | Start 0 ns; distance 3 ticks; size 8; 11 refreshes every 50 ns |
| 2 / noise | Start 5 ns; 100 actions; uniform gaps 1–9 ns; sizes 1–4; distance 0–4 ticks; buy 50%; cancel 25% |
| 3 / market-order taker | Start 15 ns; 70 orders; uniform gaps 4–12 ns; sizes 1–6; buy 50% |
| 4 / market maker | Start 0 ns; distance 1 tick; size 5; 21 refreshes every 25 ns |

The market maker is a fixed-reference baseline: it quotes at 99 and 101 ticks.
Every provider/maker refresh cancels its previous bid, cancels its previous ask,
then submits a fresh bid and fresh ask. A filled cancellation target follows
normal `NotActive` rejection. There is no automatic replenishment between
refreshes, inventory skew, market-price feedback or end-of-run flattening.

The experiment owns one explicitly seeded RNG and one shared client-ID source.
Plans are constructed in ID order (liquidity, noise, taker, maker); this fixes
source consumption and client IDs. All requests are then scheduled in that same
participant order. At equal timestamps, actual scheduling insertion sequence
resolves priority; within a participant, script order is preserved.
See [controlled participants](controlled-participants.md) for exact random draw
and plan-failure rules. No callback or producer mutates the book directly.

## Exclusive observation horizon

The CLI observes **[0, 500 ns)**. All 294 finite-plan requests are scheduled,
including refreshes at exactly 500 ns and later noise/taker actions.
Delivery stops before the earliest pending request whose time is at least 500.
The report includes the number still pending. No request is deleted, retimed
or replaced by a synthetic end marker. In particular, the four liquidity and
four maker refresh actions at 500 ns remain pending.

The horizon and last committed exchange timestamp are separate fields.
Computing statistics through 500 ns does not advance exchange time, assign
sequences, cancel final quotes or change accounts. Positions and cash change
only through executions. FIFO uses the explicit mark 100, zero-origin flows
and the existing no-fee/self-match rules.

The example's `run(seed, horizon)` also accepts an explicit logical horizon for
boundary/regression checks. It clips delivery of the same finite plans; it does
not generate additional actions for a longer horizon. If the queue drains early,
the last market state persists to the requested observation end.

The result owns delivered arrivals/events, final market/depth, participant
request counts, accounts, fills, FIFO PnL and spread statistics. Failures abort
the local example; it does not promise whole-experiment checkpoint/retry.

## Exact time-weighted spread

For each interval [t, next t), use the committed best prices after requests at t.
All transitions at an equal timestamp have zero duration; the last state at that
timestamp governs the next positive interval. The initial state has no prices.

When bid and ask exist, accumulate
`(ask_ticks - bid_ticks) * duration_ns` and covered duration. Otherwise accumulate
missing-price duration. Extend the final state to the horizon. The mean is the
exact unreduced fraction `tick_nanoseconds / covered_nanoseconds`, in ticks.
It is absent if covered duration is zero. Missing liquidity is reported
separately and never substituted with zero spread.

For example: no prices until 5 ns; spread 2 from 5 to 10; spread 6 from 10 to 20.
The report is integral 70 tick-ns, covered 15 ns, missing 5 ns, mean 70/15 ticks.
Replacing a sample several times at 10 ns changes only the final spread used
from 10 onward.

### Observer API

`<marketlab/time_weighted_spread.hpp>` exposes:

- `SpreadIntegral{high, low}`: exact nonnegative tick-nanoseconds,
  `high * 2^64 + low`; `to_string()` formats its decimal value.
- `SpreadStatistics`: integral, `covered_nanoseconds`,
  `missing_nanoseconds` and optional `MeanSpread` from `mean()`.
- `TimeWeightedSpread`: starts at t=0 with missing prices.
  `observe(time, optional<int64_t> spread_ticks)` integrates the previous
  state up to time, then replaces it. Nondecreasing timestamps and nonnegative
  spreads are required; zero is a valid observed spread.
- `snapshot(horizon)`: returns an owned projection through horizon without
  changing the observer. The horizon must be at least the last observation time.

Both operations return `std::expected`, are constexpr/noexcept and allocate
nothing. Invalid time returns `InvalidTimestamp`; otherwise a negative spread
returns `InvalidSpread`. Refused observations/reads leave all state unchanged.
The observer takes explicit samples and has no exchange, instrument, account or
simulation-clock ownership. Read between completed exchange operations under
the single writer; feed one observer from one chosen market timeline.

The full integral is below 2^127: spread is at most INT64_MAX, and disjoint
durations total at most UINT64_MAX. Products use the existing portable full
64-by-64 multiplication primitive and accumulate into two words, retaining
carries. No compiler-specific wide integer, floating-point mean, rounding or
wall-clock delay is involved. Formatting alone may allocate.

## Report and reference result

Default output has seven lines: aggregate workload/market values, spread,
four participant reports, then final depth. Order/cancellation counts include
attempts, with accepted/rejected outcomes split per participant.
A trade is one execution; [fills](participant-accounting.md#participant-fill-counts)
count participant execution legs independently of size and full-order status.
Cash/PnL units are exact ticks times quantity. Depth uses
`price_ticks:remaining_units:order_count`.

Seed 42 yields:

| Metric | Value |
|---|---:|
| Delivered / pending requests | 260 / 34 |
| New-order / cancel attempts | 186 / 74 |
| Events / trades / units traded | 682 / 99 / 223 |
| Last committed time / observation end | 496 / 500 ns |
| Best bid / ask | 97 / 101 ticks |
| Spread integral / covered / missing time | 1078 tick-ns / 500 ns / 0 ns |
| Exact mean spread | 1078/500 ticks |

| Participant | Position | Cash flow | Realised PnL | Unrealised PnL | Total PnL |
|---|---:|---:|---:|---:|---:|
| Liquidity | 0 | 0 | 0 | 0 | 0 |
| Noise | +39 | -3754 | +61 | +85 | +146 |
| Taker | -46 | +4331 | -197 | -72 | -269 |
| Market maker | +7 | -577 | +116 | +7 | +123 |

The market maker has 21 buy and 21 sell fills, all 42 passive.
The three nonzero total PnLs sum to zero at the common mark. Realised and
unrealised components need not separately sum to zero.

`--events` writes only the existing v1 event records, with no summary mixed in.
The seed-42 logical event stream (682 lines with LF endings) has FNV-1a-64
checksum `a5e016a02d517e53`, using offset 14695981039346656037 and multiplier
1099511628211. This is a regression checksum, not a cryptographic digest.
Windows stdout may translate LF to CRLF; normalize line endings before comparing
that checksum. Changing engine/configuration/draw order can intentionally change
the reference values and requires reviewing/updating the regression.

Exit codes are 0 success, 1 run/output failure and 2 invalid arguments.
See [README commands](../README.md#run-the-reference-market-making-experiment)
for Debug, event export and clean test-disabled builds.

## Validation and scope

Tests compare the complete literal summary and event checksum, repeat four
seeds (0, 42, 123, UINT64_MAX), and compare exact events, accounts, PnL, spread and
depth output. Direct exchange replay verifies scheduled delivery results.
An independent active-order reconstruction checks final depth and fill roles;
small integer cash/quantity calculations and unit-sized FIFO lots check every
account and PnL. A separate one-sample-per-nanosecond oracle checks the spread.
Tests cover zero/one-nanosecond horizons, priority at tied timestamps,
cancel-before-quote refresh, exclusion of 500 ns and extension after queue drain.

The observer tests include missing/zero spreads, tied observations, invalid
time/spread rollback, owned projections, no-allocation paths, full uint64 time
and maximum int64 spread, plus 10,000 independently reconciled decimal intervals.

This is a deterministic demo, regression workload and future benchmark fixture.
Its numbers validate implementation behavior; no throughput benchmark or
live-trading result is claimed. Multiple instruments and workers remain later
roadmap phases.
