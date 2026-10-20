#include "../benchmarks/routed_benchmark.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace bench = marketlab::benchmark;
using namespace marketlab;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

void test_latency_ranks() {
    std::array<std::int64_t, 1000> values;
    for (std::size_t index = 0; index < values.size(); ++index) values[index] = static_cast<std::int64_t>(1000 - index);
    const auto result = bench::latency(values);
    require(result.minimum == 1 && result.p50 == 500 && result.p95 == 950 && result.p99 == 990 &&
        result.p999 == 999 && result.maximum == 1000, "latency percentiles changed nearest-rank convention");
    std::array<std::int64_t, 3> small{8, 2, 5};
    const auto few = bench::latency(small);
    require(few.minimum == 2 && few.p50 == 5 && few.p95 == 8 && few.p999 == 8, "small sample percentile boundary differs");
    bool refused = false;
    try { static_cast<void>(bench::latency({})); } catch (const std::invalid_argument&) { refused = true; }
    require(refused, "empty latency input accepted");
}

void test_sink_and_workloads() {
    bench::RecordSink sink;
    std::ostream output{&sink};
    output << "hello";
    output.put('\n');
    require(sink.bytes() == 6 && sink.hash() == 0xa9bc80cca21f28b3ULL, "counting/FNV sink skipped or changed bytes");
    const auto sweep = bench::workload(bench::Scenario::Sweep, 2);
    require(sweep.size() == 8, "sweep request count differs");
    for (std::size_t index = 0; index < 3; ++index) {
        const auto& order = std::get<OrderRequest>(sweep[index]);
        require(order.side == Side::Sell && order.participant_id == ParticipantId{1} && order.order_id.value == index + 1 &&
            order.quantity_units == static_cast<std::int64_t>(index + 1) && order.limit_price_ticks == 100 + static_cast<std::int64_t>(index),
            "sweep fixture no longer establishes three distinct passive levels");
    }
    const auto& aggressive = std::get<OrderRequest>(sweep[3]);
    require(aggressive.side == Side::Buy && aggressive.participant_id == ParticipantId{2} &&
        aggressive.quantity_units == 6 && aggressive.limit_price_ticks == 102, "sweep fixture does not consume all three levels");
    for (const auto scenario : {bench::Scenario::Match, bench::Scenario::Cancel, bench::Scenario::Sweep}) {
        for (const auto cycles : {1ULL, 2ULL, 17ULL}) {
            const auto plain = bench::run(scenario, cycles, false), recorded = bench::run(scenario, cycles, true);
            const auto book_plain = bench::run(scenario, cycles, false, true);
            const auto book_recorded = bench::run(scenario, cycles, true, true);
            const auto script = bench::workload(scenario, cycles);
            std::ostringstream expected_text;
            const std::array ids{ParticipantId{1}, ParticipantId{2}};
            Exchange replay{Instrument{{1}, "BENCH", TickSize::from_decimal(1, 0).value()}, ids};
            for (std::size_t index = 0; index < script.size(); ++index) {
                require(scheduled_time(SimulationEvent{{index + 1}, script[index]}) == LogicalTime{index}, "workload time is not monotonic");
                const auto events = std::visit([&](const auto& request) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) return replay.process_order(request);
                    else return replay.process_cancel(request);
                }, script[index]);
                require(events.has_value(), "reference benchmark processing failed");
                for (const auto& event : *events) write_event_record(expected_text, event);
            }
            std::uint64_t expected_hash = 14695981039346656037ULL;
            for (const unsigned char byte : expected_text.str()) expected_hash = (expected_hash ^ byte) * 1099511628211ULL;
            require(plain.requests == recorded.requests && plain.events == recorded.events &&
                plain.executions == recorded.executions && plain.volume_units == recorded.volume_units &&
                plain.traded_tick_units == recorded.traded_tick_units && plain.record_bytes == 0 &&
                recorded.record_bytes == expected_text.str().size() && recorded.record_hash == expected_hash,
                "recording changed workload outcomes or did not encode the complete event stream");
            require(!plain.book_only && !recorded.book_only && book_plain.book_only && book_recorded.book_only,
                "benchmark engine identity is incorrect");
            const auto width = scenario == bench::Scenario::Sweep ? 4ULL : 2ULL;
            const auto events_per_cycle = scenario == bench::Scenario::Sweep ? 16ULL : scenario == bench::Scenario::Match ? 6ULL : 3ULL;
            for (const auto* result : {&plain, &recorded, &book_plain, &book_recorded}) {
                require(result->requests == cycles * width && result->events == cycles * events_per_cycle &&
                    result->executions == cycles * (scenario == bench::Scenario::Sweep ? 3ULL : scenario == bench::Scenario::Match ? 1ULL : 0ULL) &&
                    result->volume_units == cycles * (scenario == bench::Scenario::Sweep ? 6ULL : scenario == bench::Scenario::Match ? 10ULL : 0ULL) &&
                    result->traded_tick_units == cycles * (scenario == bench::Scenario::Sweep ? 608ULL : scenario == bench::Scenario::Match ? 1000ULL : 0ULL),
                    "benchmark differs from literal workload totals");
            }
            require(book_plain.record_bytes == 0 && book_recorded.record_bytes == recorded.record_bytes &&
                book_recorded.record_hash == expected_hash,
                "direct book processing changed the exchange event stream or cancellation sequences");
        }
    }
    for (const auto cycles : {0ULL, bench::maximum_cycles + 1ULL}) {
        for (const auto book_only : {false, true}) {
            bool refused = false;
            try { static_cast<void>(bench::run(bench::Scenario::Match, cycles, false, book_only)); }
            catch (const std::invalid_argument&) { refused = true; }
            require(refused, "invalid cycle count accepted");
        }
    }
    bool refused_scenario = false;
    try { static_cast<void>(bench::run(static_cast<bench::Scenario>(99), 1, false, true)); }
    catch (const std::invalid_argument&) { refused_scenario = true; }
    require(refused_scenario, "invalid book scenario accepted");
}

void test_routing_fixtures() {
    for (const auto traffic : {bench::Traffic::Balanced, bench::Traffic::Hot}) {
        const auto script = bench::routed_workload(bench::Scenario::Cancel, 40, 4, traffic);
        std::array<std::uint64_t, 4> loads{};
        for (std::size_t cycle = 0; cycle < 40; ++cycle) {
            const auto& order = std::get<OrderRequest>(script[2 * cycle]);
            const auto& cancel = std::get<CancelRequest>(script[2 * cycle + 1]);
            const auto expected = traffic == bench::Traffic::Balanced ? cycle % 4 + 1 :
                cycle == 9 || cycle == 39 ? 2ULL : cycle == 19 ? 3ULL : cycle == 29 ? 4ULL : 1ULL;
            require(order.instrument_id.value == expected && cancel.instrument_id == order.instrument_id &&
                order.order_id.value == 2 * cycle + 1 && cancel.target_order_id == order.order_id &&
                order.timestamp == LogicalTime{2 * cycle} && cancel.timestamp == LogicalTime{2 * cycle + 1},
                "routing changed the fixed request order, clock, identity or distribution");
            ++loads[order.instrument_id.value - 1];
        }
        const auto expected = traffic == bench::Traffic::Balanced ? std::array<std::uint64_t, 4>{10, 10, 10, 10} :
            std::array<std::uint64_t, 4>{36, 2, 1, 1};
        require(loads == expected, "routing does not match literal balanced/hot counts");
    }
    for (const auto count : std::array<std::uint64_t, 2>{0, bench::maximum_instruments + 1}) {
        bool refused = false;
        try { static_cast<void>(bench::run_routed(bench::Scenario::Match, 1, false, count, bench::Traffic::Balanced)); }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused, "out-of-range instrument count accepted");
    }
    for (const auto traffic : {bench::Traffic::Hot, static_cast<bench::Traffic>(99)}) {
        bool refused = false;
        try { bench::validate_routing(1, traffic); } catch (const std::invalid_argument&) { refused = true; }
        require(refused, "invalid traffic/instrument combination accepted");
    }
}

void test_routed_results() {
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    for (const auto scenario : {bench::Scenario::Match, bench::Scenario::Cancel, bench::Scenario::Sweep}) {
        for (const auto instruments : {1ULL, 4ULL, 16ULL, 256ULL}) {
            for (const auto traffic : {bench::Traffic::Balanced, bench::Traffic::Hot}) {
                if (instruments == 1 && traffic == bench::Traffic::Hot) continue;
                for (const auto cycles : {1ULL, 11ULL, 40ULL}) {
                    const auto actual = bench::run_routed(scenario, cycles, true, instruments, traffic);
                    const auto plain = bench::run_routed(scenario, cycles, false, instruments, traffic);
                    std::map<InstrumentId, Exchange> isolated;
                    for (std::uint64_t id = 1; id <= instruments; ++id) {
                        isolated.try_emplace(InstrumentId{id},
                            Instrument{{id}, "ISOLATED", TickSize::from_decimal(1, 0).value()}, participants);
                    }
                    std::ostringstream expected_text;
                    const auto script = bench::routed_workload(scenario, cycles, instruments, traffic);
                    for (const auto& request : script) {
                        const auto events = std::visit([&](const auto& value) {
                            auto& owner = isolated.at(value.instrument_id);
                            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, OrderRequest>) return owner.process_order(value);
                            else return owner.process_cancel(value);
                        }, request);
                        require(events.has_value(), "isolated benchmark replay diagnostic");
                        for (const auto& event : *events) write_event_record(expected_text, event);
                    }
                    std::uint64_t hash = 14695981039346656037ULL;
                    for (const unsigned char byte : expected_text.str()) hash = (hash ^ byte) * 1099511628211ULL;
                    const auto& total = actual.total;
                    require(actual.traffic == traffic && actual.loads.size() == instruments && !total.book_only &&
                        total.requests == script.size() && total.events == plain.total.events &&
                        total.executions == plain.total.executions && total.volume_units == plain.total.volume_units &&
                        total.traded_tick_units == plain.total.traded_tick_units && plain.total.record_bytes == 0 &&
                        total.record_bytes == expected_text.str().size() && total.record_hash == hash,
                        "routed benchmark differs from isolated events or recording changes outcomes");
                    std::uint64_t counted_cycles = 0;
                    const auto width = scenario == bench::Scenario::Sweep ? 4ULL : 2ULL;
                    for (std::size_t index = 0; index < actual.loads.size(); ++index) {
                        const auto& owner = isolated.at({index + 1});
                        require(owner.last_request_sequence().value == actual.loads[index].cycles * width,
                            "reported instrument cycle count differs from isolated requests");
                        counted_cycles += actual.loads[index].cycles;
                    }
                    require(counted_cycles == cycles, "routed cycle total differs");
                    if (instruments == 1) {
                        const auto single = bench::run(scenario, cycles, true);
                        require(total.record_bytes == single.record_bytes && total.record_hash == single.record_hash,
                            "single-instrument routing changed the original exchange workload");
                    }
                }
            }
        }
    }
}

int main() {
    try { test_latency_ranks(); test_sink_and_workloads(); test_routing_fixtures(); test_routed_results(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
