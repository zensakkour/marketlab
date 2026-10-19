#pragma once

#include <marketlab/event_record.hpp>
#include <marketlab/exchange.hpp>
#include <marketlab/simulation_event.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace marketlab::benchmark {

enum class Scenario { Match, Cancel, Sweep };
inline constexpr std::uint64_t maximum_cycles = 1'000'000;

[[nodiscard]] constexpr std::string_view name(Scenario scenario) {
    switch (scenario) {
    case Scenario::Match: return "match";
    case Scenario::Cancel: return "cancel";
    case Scenario::Sweep: return "sweep";
    }
    throw std::invalid_argument{"unknown benchmark scenario"};
}

[[nodiscard]] inline std::vector<SimulationRequest> workload(Scenario scenario, std::uint64_t cycles) {
    static_cast<void>(name(scenario));
    if (cycles == 0 || cycles > maximum_cycles) throw std::invalid_argument{"cycles must be 1 through 1000000"};
    const auto width = scenario == Scenario::Sweep ? 4ULL : 2ULL;
    std::vector<SimulationRequest> requests;
    requests.reserve(static_cast<std::size_t>(cycles * width));
    const auto order = [](std::uint64_t time, std::uint64_t id, ParticipantId participant, Side side,
                          std::int64_t price, std::int64_t units) {
        return OrderRequest{{time}, {1}, participant, {id}, side, OrderType::Limit, units, price};
    };
    for (std::uint64_t cycle = 0; cycle < cycles; ++cycle) {
        const auto time = cycle * width, id = time + 1;
        if (scenario == Scenario::Cancel) {
            requests.emplace_back(order(time, id, {1}, Side::Buy, 100, 10));
            requests.emplace_back(CancelRequest{{time + 1}, {1}, {1}, {id}});
        } else if (scenario == Scenario::Match) {
            requests.emplace_back(order(time, id, {1}, Side::Sell, 100, 10));
            requests.emplace_back(order(time + 1, id + 1, {2}, Side::Buy, 100, 10));
        } else {
            requests.emplace_back(order(time, id, {1}, Side::Sell, 100, 1));
            requests.emplace_back(order(time + 1, id + 1, {1}, Side::Sell, 101, 2));
            requests.emplace_back(order(time + 2, id + 2, {1}, Side::Sell, 102, 3));
            requests.emplace_back(order(time + 3, id + 3, {2}, Side::Buy, 102, 6));
        }
    }
    return requests;
}

// Fixed memory: the real text encoder writes every byte, then the sink counts
// and hashes it. Recorder timing includes this FNV work, with no disk I/O.
class RecordSink final : public std::streambuf {
public:
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        for (std::streamsize index = 0; index < count; ++index) append(static_cast<unsigned char>(data[index]));
        return count;
    }
    int_type overflow(int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            append(static_cast<unsigned char>(traits_type::to_char_type(value)));
        }
        return traits_type::not_eof(value);
    }

private:
    void append(unsigned char byte) noexcept {
        ++bytes_;
        hash_ = (hash_ ^ byte) * 1099511628211ULL;
    }
    std::uint64_t bytes_{};
    std::uint64_t hash_{14695981039346656037ULL};
};

struct Latency {
    std::int64_t minimum, p50, p95, p99, p999, maximum;
};

[[nodiscard]] inline Latency latency(std::span<std::int64_t> samples) {
    if (samples.empty() || samples.size() > 4 * maximum_cycles) {
        throw std::invalid_argument{"latency sample count outside benchmark bounds"};
    }
    std::ranges::sort(samples);
    const auto rank = [&](std::size_t permille) { return samples[(samples.size() * permille + 999) / 1000 - 1]; };
    return {samples.front(), rank(500), rank(950), rank(990), rank(999), samples.back()};
}

struct Result {
    Scenario scenario;
    std::uint64_t cycles;
    bool record;
    std::uint64_t requests{}, events{}, executions{}, volume_units{}, traded_tick_units{};
    std::int64_t wall_ns{};
    Latency process_ns{};
    std::uint64_t record_bytes{}, record_hash{};
};

[[nodiscard]] inline Result run(Scenario scenario, std::uint64_t cycles, bool record) {
    const auto requests = workload(scenario, cycles);
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    Exchange exchange{Instrument{{1}, "BENCH", TickSize::from_decimal(1, 0).value()}, participants};
    std::vector<std::int64_t> samples(requests.size());
    RecordSink sink;
    std::ostream output{&sink};
    Result result{scenario, cycles, record};
    std::uint64_t rejections = 0;
    using Clock = std::chrono::steady_clock;
    static_assert(Clock::is_steady);
    const auto begin = Clock::now();
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto start = Clock::now();
        auto events = std::visit([&](const auto& request) {
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) return exchange.process_order(request);
            else return exchange.process_cancel(request);
        }, requests[index]);
        const auto end = Clock::now();
        samples[index] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        if (!events) throw std::runtime_error{"benchmark exchange diagnostic"};
        ++result.requests;
        result.events += events->size();
        for (const auto& event : *events) {
            if (const auto* execution = std::get_if<Execution>(&event)) {
                ++result.executions;
                result.volume_units += static_cast<std::uint64_t>(execution->quantity.units());
                result.traded_tick_units += static_cast<std::uint64_t>(execution->price.ticks()) *
                    static_cast<std::uint64_t>(execution->quantity.units());
            }
            if (std::holds_alternative<OrderRejected>(event) || std::holds_alternative<CancelRejected>(event)) ++rejections;
            if (record) write_event_record(output, event);
        }
    }
    const auto end = Clock::now();
    result.wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    result.process_ns = latency(samples);
    result.record_bytes = sink.bytes();
    result.record_hash = sink.hash();
    const auto width = scenario == Scenario::Sweep ? 4ULL : 2ULL;
    const auto events_per_cycle = scenario == Scenario::Sweep ? 16ULL : scenario == Scenario::Match ? 6ULL : 3ULL;
    const auto fills = scenario == Scenario::Sweep ? 3ULL : scenario == Scenario::Match ? 1ULL : 0ULL;
    const auto units = scenario == Scenario::Sweep ? 6ULL : scenario == Scenario::Match ? 10ULL : 0ULL;
    const auto notional = scenario == Scenario::Sweep ? 608ULL : scenario == Scenario::Match ? 1000ULL : 0ULL;
    const auto last = exchange.last_trade();
    const bool last_valid = scenario == Scenario::Cancel ? !last : last &&
        last->aggressive_order_id.value == cycles * width && last->resting_order_id.value == cycles * width - 1 &&
        last->buyer_id == ParticipantId{2} && last->seller_id == ParticipantId{1} &&
        last->price.ticks() == (scenario == Scenario::Match ? 100 : 102) &&
        last->quantity.units() == (scenario == Scenario::Match ? 10 : 3);
    if (result.requests != cycles * width || result.events != cycles * events_per_cycle || result.executions != cycles * fills ||
        result.volume_units != cycles * units || result.traded_tick_units != cycles * notional || rejections != 0 ||
        !exchange.book().empty() || !exchange.book().check_invariants() || !last_valid ||
        exchange.last_request_sequence().value != result.requests || exchange.last_event_sequence().value != result.events ||
        exchange.current_time() != LogicalTime{result.requests - 1} ||
        to_string(exchange.cumulative_volume()) != std::to_string(result.volume_units) ||
        !output || (record && result.record_bytes == 0) || (!record && result.record_bytes != 0) || result.wall_ns <= 0) {
        throw std::runtime_error{"benchmark correctness or clock check failed"};
    }
    return result;
}

} // namespace marketlab::benchmark
