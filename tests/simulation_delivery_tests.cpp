#include <marketlab/event_record.hpp>
#include <marketlab/market_data.hpp>
#include <marketlab/simulation.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using marketlab::AcceptedOrder;
using marketlab::CancelRejected;
using marketlab::CancelRequest;
using marketlab::EventHeader;
using marketlab::ExchangeEvent;
using marketlab::Execution;
using marketlab::Instrument;
using marketlab::OrderAccepted;
using marketlab::OrderCancelled;
using marketlab::OrderFilled;
using marketlab::OrderPartiallyFilled;
using marketlab::OrderRejected;
using marketlab::OrderRequest;
using marketlab::OrderRested;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RestingOrder;
using marketlab::Side;
using marketlab::Simulation;
using marketlab::SimulationRequest;
using marketlab::TickSize;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

bool same_request(const OrderRequest& left, const OrderRequest& right) {
    return left.timestamp == right.timestamp && left.instrument_id == right.instrument_id &&
        left.participant_id == right.participant_id && left.order_id == right.order_id && left.side == right.side &&
        left.type == right.type && left.quantity_units == right.quantity_units && left.limit_price_ticks == right.limit_price_ticks;
}

bool same_request(const CancelRequest& left, const CancelRequest& right) {
    return left.timestamp == right.timestamp && left.instrument_id == right.instrument_id &&
        left.participant_id == right.participant_id && left.target_order_id == right.target_order_id;
}

bool same_order(const AcceptedOrder& left, const AcceptedOrder& right) {
    return left.order_id == right.order_id && left.participant_id == right.participant_id &&
        left.instrument_id == right.instrument_id && left.side == right.side && left.type == right.type &&
        left.original_quantity == right.original_quantity && left.limit_price == right.limit_price &&
        left.accepted_at == right.accepted_at && left.arrival_sequence == right.arrival_sequence;
}

EventHeader event_header(const ExchangeEvent& event) {
    return std::visit([](const auto& record) {
        if constexpr (std::is_same_v<std::decay_t<decltype(record)>, Execution>) {
            return EventHeader{record.timestamp, record.event_sequence, record.request_sequence, record.instrument_id};
        } else {
            return record.header;
        }
    }, event);
}

bool same_event(const ExchangeEvent& actual, const ExchangeEvent& expected) {
    if (!marketlab::is_valid(actual) || !marketlab::is_valid(expected)) {
        return false;
    }
    const auto left = event_header(actual);
    const auto right = event_header(expected);
    if (left.timestamp != right.timestamp || left.event_sequence != right.event_sequence ||
        left.request_sequence != right.request_sequence || left.instrument_id != right.instrument_id) {
        return false;
    }
    return std::visit([&](const auto& record) {
        using Record = std::decay_t<decltype(record)>;
        const auto* wanted = std::get_if<Record>(&expected);
        if (!wanted) {
            return false;
        }
        if constexpr (std::is_same_v<Record, OrderAccepted>) {
            return same_order(record.order, wanted->order);
        } else if constexpr (std::is_same_v<Record, Execution>) {
            return record.aggressive_order_id == wanted->aggressive_order_id &&
                record.resting_order_id == wanted->resting_order_id && record.aggressive_side == wanted->aggressive_side &&
                record.buyer_id == wanted->buyer_id && record.seller_id == wanted->seller_id &&
                record.price == wanted->price && record.quantity == wanted->quantity;
        } else if constexpr (std::is_same_v<Record, OrderRejected> || std::is_same_v<Record, CancelRejected>) {
            return same_request(record.request, wanted->request) && record.reason == wanted->reason;
        } else {
            if (record.order_id != wanted->order_id || record.participant_id != wanted->participant_id) {
                return false;
            }
            if constexpr (std::is_same_v<Record, OrderCancelled>) {
                return record.price == wanted->price && record.cancelled_quantity == wanted->cancelled_quantity;
            } else if constexpr (std::is_same_v<Record, OrderRested>) {
                return record.price == wanted->price && record.remaining_quantity == wanted->remaining_quantity;
            } else {
                return record.remaining_quantity == wanted->remaining_quantity;
            }
        }
    }, actual);
}

Quantity quantity(std::int64_t units) {
    return Quantity::from_units(units).value();
}

AcceptedOrder accepted_order(const OrderRequest& request, std::uint64_t sequence) {
    return {request.order_id, request.participant_id, request.instrument_id, request.side, request.type,
        quantity(request.quantity_units), Price::from_ticks(*request.limit_price_ticks), request.timestamp, {sequence}};
}

struct ScriptResult {
    std::vector<ExchangeEvent> events;
    RestingOrder survivor;
    std::string recording;
    std::string market_data;
};

constexpr std::string_view expected_script_record =
    "v=1 type=OrderAccepted time_ns=1 event_seq=1 request_seq=1 instrument_id=1 order_id=900 participant_id=1 "
    "side=1 order_type=1 original_units=5 limit_ticks=100\n"
    "v=1 type=OrderRested time_ns=1 event_seq=2 request_seq=1 instrument_id=1 order_id=900 participant_id=1 "
    "price_ticks=100 remaining_units=5\n"
    "v=1 type=OrderAccepted time_ns=1 event_seq=3 request_seq=2 instrument_id=1 order_id=1 participant_id=2 "
    "side=1 order_type=1 original_units=3 limit_ticks=100\n"
    "v=1 type=OrderRested time_ns=1 event_seq=4 request_seq=2 instrument_id=1 order_id=1 participant_id=2 "
    "price_ticks=100 remaining_units=3\n"
    "v=1 type=OrderAccepted time_ns=2 event_seq=5 request_seq=3 instrument_id=1 order_id=2 participant_id=2 "
    "side=2 order_type=1 original_units=5 limit_ticks=101\n"
    "v=1 type=OrderRested time_ns=2 event_seq=6 request_seq=3 instrument_id=1 order_id=2 participant_id=2 "
    "price_ticks=101 remaining_units=5\n"
    "v=1 type=OrderAccepted time_ns=3 event_seq=7 request_seq=4 instrument_id=1 order_id=50 participant_id=1 "
    "side=1 order_type=1 original_units=2 limit_ticks=101\n"
    "v=1 type=Execution time_ns=3 event_seq=8 request_seq=4 instrument_id=1 aggressive_order_id=50 "
    "resting_order_id=2 aggressive_side=1 buyer_id=1 seller_id=2 price_ticks=101 quantity_units=2\n"
    "v=1 type=OrderPartiallyFilled time_ns=3 event_seq=9 request_seq=4 instrument_id=1 order_id=2 "
    "participant_id=2 remaining_units=3\n"
    "v=1 type=OrderFilled time_ns=3 event_seq=10 request_seq=4 instrument_id=1 order_id=50 participant_id=1 "
    "remaining_units=0\n"
    "v=1 type=OrderCancelled time_ns=4 event_seq=11 request_seq=5 instrument_id=1 order_id=2 participant_id=2 "
    "price_ticks=101 cancelled_units=3\n"
    "v=1 type=OrderCancelled time_ns=4 event_seq=12 request_seq=6 instrument_id=1 order_id=900 participant_id=1 "
    "price_ticks=100 cancelled_units=5\n";

void write_market_snapshot(std::ostream& output, const marketlab::MarketSnapshot& snapshot) {
    const auto& prices = snapshot.prices;
    output << "instrument_id=" << prices.instrument_id.value << " time_ns=" << prices.timestamp.nanoseconds
           << " request_seq=" << prices.last_request_sequence.value
           << " event_seq=" << prices.last_event_sequence.value << " bid_ticks=";
    if (prices.bid) output << prices.bid->ticks(); else output << "none";
    output << " ask_ticks=";
    if (prices.ask) output << prices.ask->ticks(); else output << "none";
    output << " spread_ticks=";
    if (const auto spread = prices.spread_ticks()) output << *spread; else output << "none";
    output << " mid_ticks=";
    if (const auto mid = prices.mid_price()) {
        output << mid->whole_ticks;
        if (mid->half_tick) output << ".5";
    } else {
        output << "none";
    }
    if (const auto& trade = snapshot.last_trade) {
        output << " last_ticks=" << trade->price.ticks() << " last_units=" << trade->quantity.units()
               << " last_time_ns=" << trade->timestamp.nanoseconds << " last_event_seq=" << trade->event_sequence.value;
    } else {
        output << " last_trade=none";
    }
    output << '\n';
}

constexpr std::string_view expected_market_script =
    "instrument_id=1 time_ns=1 request_seq=1 event_seq=2 bid_ticks=100 ask_ticks=none "
    "spread_ticks=none mid_ticks=none last_trade=none\n"
    "instrument_id=1 time_ns=1 request_seq=2 event_seq=4 bid_ticks=100 ask_ticks=none "
    "spread_ticks=none mid_ticks=none last_trade=none\n"
    "instrument_id=1 time_ns=2 request_seq=3 event_seq=6 bid_ticks=100 ask_ticks=101 "
    "spread_ticks=1 mid_ticks=100.5 last_trade=none\n"
    "instrument_id=1 time_ns=3 request_seq=4 event_seq=10 bid_ticks=100 ask_ticks=101 "
    "spread_ticks=1 mid_ticks=100.5 last_ticks=101 last_units=2 last_time_ns=3 last_event_seq=8\n"
    "instrument_id=1 time_ns=4 request_seq=5 event_seq=11 bid_ticks=100 ask_ticks=none "
    "spread_ticks=none mid_ticks=none last_ticks=101 last_units=2 last_time_ns=3 last_event_seq=8\n"
    "instrument_id=1 time_ns=4 request_seq=6 event_seq=12 bid_ticks=100 ask_ticks=none "
    "spread_ticks=none mid_ticks=none last_ticks=101 last_units=2 last_time_ns=3 last_event_seq=8\n";

ScriptResult run_script(bool record_events = false) {
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    Simulation simulation{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    const auto& exchange = simulation.exchange();
    const auto& queue = simulation.queue();
    const OrderRequest bid{{1}, {1}, {1}, {900}, Side::Buy, OrderType::Limit, 5, 100};
    const OrderRequest later_bid{{1}, {1}, {2}, {1}, Side::Buy, OrderType::Limit, 3, 100};
    const OrderRequest ask{{2}, {1}, {2}, {2}, Side::Sell, OrderType::Limit, 5, 101};
    const OrderRequest crossing{{3}, {1}, {1}, {50}, Side::Buy, OrderType::Limit, 2, 101};
    // Scheduling insertion is deliberately different from chronological delivery.
    const std::array<SimulationRequest, 6> requests{
        CancelRequest{{4}, {1}, {2}, {2}}, crossing, bid, ask, later_bid,
        CancelRequest{{4}, {1}, {1}, {900}},
    };
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto sequence = simulation.schedule(requests[index]);
        require(sequence && sequence->value == index + 1, "script scheduling sequence differs");
    }
    constexpr std::array<std::uint64_t, 6> delivery_sequence{3, 5, 4, 2, 1, 6};
    constexpr std::array<std::uint64_t, 6> bid_heads{900, 900, 900, 900, 900, 1};
    constexpr std::array<std::uint64_t, 6> ask_heads{0, 0, 2, 2, 0, 0};
    constexpr std::array<std::uint64_t, 6> event_counts{2, 4, 6, 10, 11, 12};
    std::vector<ExchangeEvent> history;
    std::ostringstream recording;
    std::ostringstream market_data;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto* arrival = queue.next();
        require(arrival && arrival->sequence.value == delivery_sequence[index], "script arrival ordering differs");
        const auto time = marketlab::scheduled_time(*arrival);
        auto result = simulation.step();
        require(result && result->has_value(), "script simulation delivery failed");
        const auto& delivered = result->value();
        require(std::visit([&](const auto& request) {
            const auto* original = std::get_if<std::decay_t<decltype(request)>>(&requests[delivery_sequence[index] - 1]);
            return original && same_request(request, *original);
        }, delivered.arrival.request), "script returned arrival payload differs");
        require(delivered.arrival.sequence.value == delivery_sequence[index] &&
            queue.size() == requests.size() - index - 1, "script pending arrivals differ");
        const auto* best_bid = exchange.book().best_bid();
        const auto* best_ask = exchange.book().best_ask();
        require(best_bid && best_bid->order.order_id.value == bid_heads[index] &&
            (best_ask ? best_ask->order.order_id.value : 0) == ask_heads[index], "script FIFO heads differ");
        require(exchange.book().check_invariants() && exchange.current_time() == time &&
            exchange.last_request_sequence().value == index + 1 &&
            exchange.last_event_sequence().value == event_counts[index], "script book or committed clock/counters differ");
        if (record_events) {
            for (const auto& event : delivered.events) marketlab::write_event_record(recording, event);
            require(recording.good(), "script recording failed");
            write_market_snapshot(market_data, marketlab::market_snapshot(exchange));
            require(market_data.good(), "script market snapshot output failed");
        }
        history.insert(history.end(), delivered.events.begin(), delivered.events.end());
    }
    const auto header = [](std::uint64_t time, std::uint64_t event, std::uint64_t request) {
        return EventHeader{{time}, {event}, {request}, {1}};
    };
    const std::vector<ExchangeEvent> expected{
        OrderAccepted{header(1, 1, 1), accepted_order(bid, 1)},
        OrderRested{header(1, 2, 1), {900}, {1}, Price::from_ticks(100).value(), quantity(5)},
        OrderAccepted{header(1, 3, 2), accepted_order(later_bid, 2)},
        OrderRested{header(1, 4, 2), {1}, {2}, Price::from_ticks(100).value(), quantity(3)},
        OrderAccepted{header(2, 5, 3), accepted_order(ask, 3)},
        OrderRested{header(2, 6, 3), {2}, {2}, Price::from_ticks(101).value(), quantity(5)},
        OrderAccepted{header(3, 7, 4), accepted_order(crossing, 4)},
        Execution{{3}, {8}, {4}, {1}, {50}, {2}, Side::Buy, {1}, {2}, Price::from_ticks(101).value(), quantity(2)},
        OrderPartiallyFilled{header(3, 9, 4), {2}, {2}, quantity(3)},
        OrderFilled{header(3, 10, 4), {50}, {1}, quantity(0)},
        OrderCancelled{header(4, 11, 5), {2}, {2}, Price::from_ticks(101).value(), quantity(3)},
        OrderCancelled{header(4, 12, 6), {900}, {1}, Price::from_ticks(100).value(), quantity(5)},
    };
    require(std::equal(history.begin(), history.end(), expected.begin(), expected.end(), same_event),
        "script ordered exchange records differ");
    const auto* survivor = exchange.book().find({1});
    require(queue.empty() && !queue.next() && queue.last_sequence().value == 6 &&
        exchange.book().order_count() == 1 && survivor && same_order(survivor->order, accepted_order(later_bid, 2)) &&
        survivor->remaining_quantity.units() == 3 && !exchange.book().find({900}) && !exchange.book().find({2}) &&
        !exchange.book().find({50}), "script final active state differs");
    require(record_events ? recording.str() == expected_script_record : recording.str().empty(),
        "script serialized events differ");
    require(record_events ? market_data.str() == expected_market_script : market_data.str().empty(),
        "script market snapshots differ");
    return {std::move(history), *survivor, recording.str(), market_data.str()};
}

bool test_lifecycle() {
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    Simulation simulation{Instrument{{1}, "TEST", TickSize::from_decimal(1, 0).value()}, participants};
    static_assert(!std::is_copy_constructible_v<Simulation>);
    static_assert(!std::is_move_constructible_v<Simulation>);
    const auto& exchange = simulation.exchange();
    const auto& queue = simulation.queue();
    const auto empty = simulation.step();
    if (!empty || empty->has_value() || simulation.current_time().nanoseconds != 0 ||
        exchange.last_request_sequence().value != 0 || exchange.last_event_sequence().value != 0) return false;
    const OrderRequest malformed{{10}, {1}, {1}, {99}, Side::Buy, OrderType::Limit, 0, 100};
    const auto first_sequence = simulation.schedule(malformed);
    if (!first_sequence || first_sequence->value != 1 || simulation.current_time().nanoseconds != 0) return false;
    const auto first = simulation.step();
    if (!first || !*first || first->value().arrival.sequence.value != 1 || first->value().events.size() != 1) return false;
    const auto* rejected = std::get_if<OrderRejected>(&first->value().events.front());
    if (!rejected || rejected->reason != marketlab::RejectionReason::InvalidQuantity ||
        !same_request(rejected->request, malformed) ||
        !same_request(std::get<OrderRequest>(first->value().arrival.request), malformed) ||
        simulation.current_time().nanoseconds != 10 || !queue.empty() ||
        exchange.last_request_sequence().value != 1 || exchange.last_event_sequence().value != 1) return false;
    const OrderRequest past_order{{9}, {1}, {1}, {99}, Side::Buy, OrderType::Limit, 3, 100};
    const CancelRequest past_cancel{{9}, {1}, {1}, {99}};
    const auto order_error = simulation.schedule(past_order);
    const auto cancel_error = simulation.schedule(past_cancel);
    if (order_error || order_error.error() != marketlab::SimulationScheduleError::InvalidTimestamp ||
        cancel_error || cancel_error.error() != marketlab::SimulationScheduleError::InvalidTimestamp ||
        queue.last_sequence().value != 1 || !queue.empty() || simulation.current_time().nanoseconds != 10 ||
        exchange.last_request_sequence().value != 1 || exchange.last_event_sequence().value != 1) return false;
    const auto drained = simulation.step();
    if (!drained || *drained || simulation.current_time().nanoseconds != 10) return false;
    // A rejected submission did not reserve its ID; resume at the current timestamp.
    const OrderRequest valid{{10}, {1}, {1}, {99}, Side::Buy, OrderType::Limit, 3, 100};
    const auto resumed = simulation.schedule(valid);
    const auto processed = simulation.step();
    if (!resumed || resumed->value != 2 || !processed || !*processed ||
        processed->value().arrival.sequence.value != 2 || processed->value().events.size() != 2 ||
        !std::holds_alternative<OrderAccepted>(processed->value().events.front()) || !exchange.book().find({99}) ||
        exchange.last_request_sequence().value != 2 || exchange.last_event_sequence().value != 3) return false;
    const CancelRequest missing{{10}, {1}, {1}, {123}};
    const auto cancellation = simulation.schedule(missing);
    const auto cancel_step = simulation.step();
    if (!cancellation || cancellation->value != 3 || !cancel_step || !*cancel_step ||
        cancel_step->value().events.size() != 1) return false;
    const auto* cancel_rejection = std::get_if<CancelRejected>(&cancel_step->value().events.front());
    const auto done = simulation.step();
    if (!cancel_rejection || cancel_rejection->reason != marketlab::RejectionReason::NotActive ||
        !same_request(cancel_rejection->request, missing) || !done || *done || !queue.empty() ||
        queue.last_sequence().value != 3 || simulation.current_time().nanoseconds != 10 ||
        exchange.last_request_sequence().value != 3 || exchange.last_event_sequence().value != 4) return false;
    constexpr auto maximum_time = std::numeric_limits<std::uint64_t>::max();
    const auto final_sequence = simulation.schedule(OrderRequest{.timestamp = {maximum_time}});
    const auto final = simulation.step();
    if (!final_sequence || final_sequence->value != 4 || !final || !*final || final->value().events.size() != 1 ||
        !marketlab::is_valid(final->value().events.front()) ||
        simulation.current_time().nanoseconds != maximum_time) return false;
    const auto* final_rejection = std::get_if<OrderRejected>(&final->value().events.front());
    if (!final_rejection || final_rejection->reason != marketlab::RejectionReason::InvalidInstrument) return false;
    const auto refused = simulation.schedule(CancelRequest{.timestamp = {maximum_time - 1}});
    const auto equal = simulation.schedule(CancelRequest{{maximum_time}, {1}, {1}, {99}});
    const auto last = simulation.step();
    const auto finished = simulation.step();
    return !refused && refused.error() == marketlab::SimulationScheduleError::InvalidTimestamp && equal &&
        equal->value == 5 && last && *last && last->value().events.size() == 1 &&
        std::holds_alternative<OrderCancelled>(last->value().events.front()) && finished && !*finished &&
        simulation.current_time().nanoseconds == maximum_time && queue.empty() && queue.last_sequence().value == 5 &&
        exchange.last_request_sequence().value == 5 && exchange.last_event_sequence().value == 6 &&
        exchange.book().order_count() == 0 && exchange.book().check_invariants();
}

int main(int argc, char** argv) {
    const bool print_record = argc == 2 && std::string_view{argv[1]} == "--record-script";
    const bool print_market = argc == 2 && std::string_view{argv[1]} == "--market-script";
    if (argc != 1 && !print_record && !print_market) {
        std::cerr << "usage: simulation_delivery_tests [--record-script|--market-script]\n";
        return 2;
    }
    try {
        require(test_lifecycle(), "simulation lifecycle, past scheduling, or rejection delivery differs");
        const auto reference = run_script();
        std::string recording;
        std::string market_data;
        for (int repeat = 0; repeat < 20; ++repeat) {
            auto replay = run_script(true);
            require(std::equal(reference.events.begin(), reference.events.end(), replay.events.begin(), replay.events.end(), same_event),
                "script replay event/execution sequence differs");
            require(same_order(reference.survivor.order, replay.survivor.order) &&
                reference.survivor.remaining_quantity == replay.survivor.remaining_quantity, "script replay final book differs");
            recording = std::move(replay.recording);
            market_data = std::move(replay.market_data);
        }
        if (print_record || print_market) {
            const auto& output = print_market ? market_data : recording;
            std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
            std::cout.flush();
            if (!std::cout) return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
