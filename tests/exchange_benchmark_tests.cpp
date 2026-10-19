#include "../benchmarks/exchange_benchmark.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
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
        }
    }
    for (const auto cycles : {0ULL, bench::maximum_cycles + 1ULL}) {
        bool refused = false;
        try { static_cast<void>(bench::workload(bench::Scenario::Match, cycles)); }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused, "invalid cycle count accepted");
    }
}

int main() {
    try { test_latency_ranks(); test_sink_and_workloads(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
