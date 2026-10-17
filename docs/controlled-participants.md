# Controlled participants

Phase 9 provides bounded, deterministic request producers using the existing
`Simulation::schedule()` and `Simulation::step()` path. The experiment owns
configuration, random state, client IDs, scheduling order and accounting.
Producers prepare scripts before the run; they do not react to market updates.

## Experiment-owned sources

`SimulationRandom(seed)` from `<marketlab/simulation_random.hpp>` owns one
`std::mt19937_64` initialized from an explicit unsigned 64-bit seed.
`uniform(minimum, maximum)` returns an inclusive unsigned sample, or an empty
optional for reversed bounds. Invalid and singleton intervals consume no engine
word; the full uint64 interval consumes one raw word. Other intervals use
rejection sampling before modulo to avoid bias from incomplete residue buckets.
Rejected words are consumed deterministically.

No clock, random device, global state or floating-point distribution is used.
The same seed and ordered calls produce identical samples. Bounds and call order
are part of reproducibility. Tests cover raw engine agreement, power-of-two
mapping, a rejection-sampling golden and repeated boundary workloads. Occupancy
checks are smoke tests, not proof of randomness.

`OrderIdSource(first)` from `<marketlab/order_id_source.hpp>` supplies client
order IDs, starting at 1 by default. Zero throws `std::invalid_argument`.
`peek()` observes the next ID; `next()` consumes it. UINT64_MAX is usable once,
then exhaustion returns an empty optional forever. Sampling, ID consumption and
source copy/assignment allocate nothing and are `noexcept`.

Copies are value checkpoints, not disjoint random streams or ID ranges. Use one
coordinated ID source across producers in an exchange, starting beyond manually
assigned IDs. The exchange retains lifetime-duplicate validation. These sources
do not assign simulation, request or exchange-event sequences.

## Owning scripted participant

`ScriptedParticipant(id, span)` from `<marketlab/scripted_participant.hpp>`
copies a list of `SimulationRequest` values. Construction requires a nonzero
owner, that owner on every request and nondecreasing logical timestamps.
Violations throw `std::invalid_argument`; copying the script may allocate.
Other order fields remain raw for exchange validation, including invalid fields.
The participant cannot be copied or moved.

`next()` borrows the current request, or returns nullptr when exhausted. Stored
actions never change, so borrowed records remain valid until destruction.
`participant_id()`, `submitted_count()`, `remaining()` and `empty()` expose
read-only progress. The caller may discard its original list after construction.

`schedule_next(simulation)` returns
`expected<optional<SimulationSequence>, SimulationScheduleError>`.
An empty optional means the script is exhausted. Otherwise it schedules exactly
one request and advances only after successful insertion. A scheduling error or
allocation exception leaves the cursor and request available for retry.
Scheduling does not process the exchange. A successfully scheduled action stays
consumed even if the exchange later rejects it.

Script order resolves equal-time requests from one producer. Across producers,
the experiment's actual scheduling insertion order resolves ties. Client IDs do
not determine priority. A script must be scheduled before the simulation passes
its next timestamp; past-time refusal is the existing scheduler's rule.

## Plan builders

`<marketlab/participant_plans.hpp>` defines `ParticipantPlan`, an owned vector
of raw requests, and three builders. Pass each result to a scripted participant,
then schedule through the normal simulation path.

All builders validate configuration even for an empty plan. They return
`expected<ParticipantPlan, ParticipantPlanError>`:
`InvalidConfiguration`, `CapacityExceeded`, `TimeOverflow` or
`OrderIdExhausted`. Allocation exceptions propagate. They prepare the entire
plan against value checkpoints and commit source states only on success.
Any diagnostic or allocation failure leaves caller RNG/IDs unchanged for retry.
Successful empty plans consume no IDs or random words and allocate nothing.

Configurations require nonzero instrument/participant IDs and positive sizes.
The exchange still validates registration and all submitted fields. Prices are
whole ticks and time is unsigned logical nanoseconds; overflow never wraps.
Plans require memory proportional to their action count and are intended for
bounded experiments, with no throughput claim.

### Noise flow

`make_noise_script(NoiseConfig, random, ids)` creates limit orders and cancels
around an explicit fixed `reference_price`.

`NoiseConfig::flow` is an `OrderFlowConfig`:

| Field | Meaning |
|---|---|
| `instrument_id`, `participant_id` | Request destination and owner |
| `first_time` | Exact time of the first action |
| `action_count` | Total number of new orders plus cancels |
| `minimum_interval_ns`, `maximum_interval_ns` | Inclusive uniform gap between subsequent actions; minimum at least 1 |
| `minimum_quantity`, `maximum_quantity` | Inclusive uniform positive order size |
| `buy_basis_points` | Buy probability from 0 through 10000 |

`maximum_distance_ticks` is nonnegative. Distances are uniform from zero through
that maximum. Buy prices are reference minus distance; sell prices are reference
plus distance. Both extreme prices must fit positive int64 ticks, even when one
side has zero probability. `cancel_basis_points` is from 0 through 10000.

The plan keeps an insertion-ordered list of its previously created orders that
have not yet been selected for cancellation. If it is empty, the next action is
a new order. Otherwise a cancellation coin is drawn; a cancellation selects one
candidate uniformly, emits its owner's cancel and removes it from that list.
Candidates describe planned orders, not live exchange state. A target already
filled by delivery time produces the normal `NotActive` rejection.

Random draw order is: subsequent-action gap; cancellation decision if eligible;
then candidate selection for a cancel, or side, quantity and distance for a new
order. Probability endpoints consume no word. Singleton uniform ranges consume
no word. A new order consumes one client ID; cancels consume none.

### Refreshed liquidity

`make_liquidity_script(LiquidityProviderConfig, ids)` uses no RNG.
Configuration supplies IDs, reference price, strictly positive
`quote_distance_ticks`, positive `quote_quantity`, `first_time`, positive
`refresh_interval_ns` and `refresh_count`. Both quotes must fit positive int64
ticks. Zero refreshes produces an empty plan.

The first refresh submits a bid at reference minus distance, then an ask at
reference plus distance. Every later refresh has this exact equal-time order:
cancel the previous bid, cancel the previous ask, submit a new bid, submit a new
ask. Each pair uses two fresh IDs, including after fills. Stale cancellations
follow normal exchange rejection rules. The final pair remains in the exchange;
there is no implicit final cleanup. For N > 0 refreshes there are 2 + 4(N - 1)
requests. This is fixed-reference background liquidity; reactive market-making
and inventory-dependent quoting remain later strategy work. The
[reference experiment](reference-market-making.md) uses a separate fixed-reference
market maker alongside background liquidity.

### Market-order taker

`make_taker_script(OrderFlowConfig, random, ids)` uses the same flow validation,
exact first time, uniform positive gaps, side probability and uniform quantity.
Every action is a market order with no limit price and one fresh ID. Draw order
is subsequent-action gap, then side and quantity. The exchange determines fills
and expires any unfilled remainder under its existing rules.

## Integrated executable and regression

`controlled_market [--events|--fills] [seed]` runs the three plans on instrument 1
(`CONTROL`, tick size 0.01), with reference and explicit FIFO valuation mark
100 ticks. Default seed is 42; decimal seeds 0 through UINT64_MAX are accepted.

| Participant | Configuration |
|---|---|
| 1: liquidity | Start 0 ns, distance 2 ticks, size 8, 8 refreshes every 50 ns |
| 2: noise | Start 5 ns, 60 actions, gaps 1–9 ns, sizes 1–4, distance 0–4 ticks, buy 50%, cancel 25% |
| 3: taker | Start 15 ns, 30 orders, gaps 4–12 ns, sizes 1–6, buy 50% |

Plans are constructed in provider/noise/taker order using shared IDs and RNG.
All provider requests are scheduled first, then noise, then taker; this also
defines cross-participant equal-time priority. Delivery itself remains ordered
by timestamp and scheduling sequence. The simulation drains its 120 arrivals,
checks book invariants after each step and feeds only executions to the ledger.

The default output reports request/event counts, new-order and cancellation
attempts (including rejections), executions, time, volume, best prices, each
participant's position/cash/FIFO PnL and final depth. Depth entries are
`price_ticks:remaining_units:order_count`. Amounts use exact tick-times-quantity
units; cash is zero-origin flow. `--events` instead writes the existing v1 event
format, with no summary mixed into it. `--fills` instead prints each
participant's buy/sell/aggressive/passive execution-leg counts; see
[fill semantics](participant-accounting.md#participant-fill-counts).
Exit codes: 0 success, 1 run/output failure, 2 invalid arguments.
A run failure aborts the example; it is not a whole-run checkpoint or event replay tool.

Seed 42 yields 120 requests, 317 events, 45 executions and 91 units traded.
Positions are +8, +5 and -13; cash flows are -720, -445 and +1165; total PnLs
at mark 100 are +80, +55 and -135. The full literal summary is a regression test.
Four seeds, including zero and UINT64_MAX, are repeated to compare event bytes,
account/PnL values and final depth. Independent event reconciliation reconstructs
active orders, price levels, quantities, cash and marked PnL. Allocation injection
tests cover every plan allocation and script construction/scheduling retry.

See [the README](../README.md#run-the-controlled-market) for build/run commands.
