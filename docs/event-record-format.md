# Event record format v1

`write_event_record(std::ostream&, const ExchangeEvent&)` writes one intrinsic
exchange event per line. It is an observer of events already returned by the
exchange or `Simulation::step()`. It owns neither the simulation nor the output
stream and preserves the caller's event order.

## Representation

Each line starts with `v=1`, followed by space-separated `key=value` fields.
Names and values are ASCII. Integers are decimal, with no grouping or padding;
prices are exact tick counts and quantities are integer units. The writer uses
`std::to_chars` and unformatted stream writes, so locale, hexadecimal flags,
width and fill do not change the representation or get consumed.

Every line contains these fields in order:

| Field | Meaning |
| --- | --- |
| `v` | Format version, currently `1`. |
| `type` | One of the nine event names below. |
| `time_ns` | Committed logical timestamp in unsigned nanoseconds. |
| `event_seq` | Exchange event sequence. |
| `request_seq` | Exchange processing sequence, distinct from simulation sequence. |
| `instrument_id` | Event instrument; a rejection may retain zero/unknown submitted IDs. |

Fields specific to each type follow in the listed order:

| Type | Additional fields |
| --- | --- |
| `OrderAccepted` | `order_id participant_id side order_type original_units limit_ticks` |
| `Execution` | `aggressive_order_id resting_order_id aggressive_side buyer_id seller_id price_ticks quantity_units` |
| `OrderPartiallyFilled` | `order_id participant_id remaining_units` |
| `OrderFilled` | `order_id participant_id remaining_units` (zero) |
| `OrderRested` | `order_id participant_id price_ticks remaining_units` |
| `MarketRemainderExpired` | `order_id participant_id remaining_units` |
| `OrderCancelled` | `order_id participant_id price_ticks cancelled_units` |
| `OrderRejected` | `submitted_ns participant_id order_id side order_type quantity_units limit_ticks reason` |
| `CancelRejected` | `submitted_ns participant_id target_order_id reason` |

Side values are `1` for buy and `2` for sell; order types are `1` for limit and
`2` for market. Rejected requests retain their raw numeric enum values, IDs and
signed quantities. `limit_ticks=none` means absent; `limit_ticks=0` retains a
present invalid price. These are different inputs.

Accepted-order instrument, acceptance time and arrival request sequence equal
`instrument_id`, `time_ns` and `request_seq` by intrinsic validation, so those
fields also preserve the original acceptance metadata. Rejection instrument
equals its raw request instrument; `submitted_ns` preserves the original request
time, including when it precedes committed `time_ns`.

`reason` uses the exact `RejectionReason` name: `InvalidTimestamp`,
`InvalidInstrument`, `InvalidParticipant`, `InvalidOrderId`, `InvalidSide`,
`InvalidOrderType`, `InvalidQuantity`, `InvalidPrice`, `DuplicateOrderId`,
`NotActive` or `NotOwner`.

The writer appends a logical newline. A text stream may use platform line
endings (LF or CRLF); a binary file stream retains LF. It does not flush each line.

## Example

The scripted scenario's execution is:

```text
v=1 type=Execution time_ns=3 event_seq=8 request_seq=4 instrument_id=1 aggressive_order_id=50 resting_order_id=2 aggressive_side=1 buyer_id=1 seller_id=2 price_ticks=101 quantity_units=2
```

Order 50 buys two units from resting order 2 at 101 ticks. Both participant IDs
and both order IDs are retained; an execution remains one trade, including
permitted self-matches.

## Validation and output failures

An intrinsically invalid event throws `std::invalid_argument` before writing
any bytes. Malformed raw requests inside valid rejection events are preserved.
The writer does not revalidate exchange decisions, assign identities, sort
events, mutate source records or inspect book state.

Output errors follow the stream's error-state/exception policy. Check the stream
and any final flush/close; a failed write can leave a partial record. The already
committed exchange operation is unchanged. Keep its returned step/events while
handling output failure; retrying output must not resubmit the arrival.

## Scope

The format records complete intrinsic exchange-event fields, not an experiment
configuration or an input schedule. Tick-size configuration and simulation
identities are separate metadata. Loading recordings, seeking and replay tools
belong to later phases.

`event_record` tests all nine types, rejection names/raw payloads, integer limits,
stream settings and output errors. `simulation_delivery` compares twelve exact
script lines and repeats the scenario twenty times with recording enabled
against a run without it, checking all events and the final book. See the
[README](../README.md#record-the-scripted-timeline) for display/export commands.
