#pragma once

#include "exchange_benchmark.hpp"

#include <marketlab/multi_instrument_exchange.hpp>

namespace marketlab::benchmark {

inline constexpr std::uint64_t maximum_instruments = 256;
enum class Traffic { Balanced, Hot };

inline void validate_routing(std::uint64_t instruments, Traffic traffic) {
    if (instruments == 0 || instruments > maximum_instruments ||
        (traffic != Traffic::Balanced && traffic != Traffic::Hot) ||
        (traffic == Traffic::Hot && instruments < 2)) {
        throw std::invalid_argument{"routing requires 1 through 256 instruments; hot traffic requires at least 2"};
    }
}

[[nodiscard]] inline std::vector<SimulationRequest> routed_workload(
    Scenario scenario, std::uint64_t cycles, std::uint64_t instruments, Traffic traffic) {
    validate_routing(instruments, traffic);
    auto requests = workload(scenario, cycles);
    const auto width = scenario == Scenario::Sweep ? 4ULL : 2ULL;
    for (std::uint64_t cycle = 0; cycle < cycles; ++cycle) {
        const auto target = traffic == Traffic::Balanced ? cycle % instruments :
            cycle % 10 < 9 ? 0 : 1 + (cycle / 10) % (instruments - 1);
        // A complete cycle stays on one book; routing does not change its fills.
        for (std::uint64_t offset = 0; offset < width; ++offset) {
            std::visit([&](auto& request) { request.instrument_id = {target + 1}; }, requests[cycle * width + offset]);
        }
    }
    return requests;
}

struct InstrumentLoad {
    std::uint64_t cycles{}, last_cycle{};
};

struct RoutedResult {
    Result total;
    Traffic traffic;
    std::vector<InstrumentLoad> loads; // Increasing instrument ID; includes idle books.
};

[[nodiscard]] inline RoutedResult run_routed(
    Scenario scenario, std::uint64_t cycles, bool record, std::uint64_t instruments, Traffic traffic) {
    const auto requests = routed_workload(scenario, cycles, instruments, traffic);
    std::vector<Instrument> configurations;
    for (std::uint64_t id = 1; id <= instruments; ++id) {
        configurations.push_back(Instrument{{id}, "BENCH" + std::to_string(id), TickSize::from_decimal(1, 0).value()});
    }
    const InstrumentRegistry registry{configurations};
    const std::array participants{ParticipantId{1}, ParticipantId{2}};
    MultiInstrumentExchange exchange{registry, participants};
    RoutedResult result{Result{scenario, cycles, record, false}, traffic, std::vector<InstrumentLoad>(instruments)};
    auto& total = result.total;
    const auto width = scenario == Scenario::Sweep ? 4ULL : 2ULL;
    for (std::uint64_t cycle = 0; cycle < cycles; ++cycle) {
        const auto id = std::visit([](const auto& request) { return request.instrument_id; }, requests[cycle * width]);
        auto& load = result.loads[id.value - 1];
        ++load.cycles;
        load.last_cycle = cycle;
    }
    std::vector<std::int64_t> samples(requests.size());
    RecordSink sink;
    std::ostream output{&sink};
    std::uint64_t rejections = 0;
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto start = Clock::now();
        auto events = std::visit([&](const auto& request) {
            if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) return exchange.process_order(request);
            else return exchange.process_cancel(request);
        }, requests[index]);
        const auto end = Clock::now();
        samples[index] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        if (!events) throw std::runtime_error{"routed benchmark diagnostic"};
        ++total.requests;
        total.events += events->size();
        for (const auto& event : *events) {
            if (const auto* execution = std::get_if<Execution>(&event)) {
                ++total.executions;
                total.volume_units += static_cast<std::uint64_t>(execution->quantity.units());
                total.traded_tick_units += static_cast<std::uint64_t>(execution->price.ticks()) *
                    static_cast<std::uint64_t>(execution->quantity.units());
            }
            if (std::holds_alternative<OrderRejected>(event) || std::holds_alternative<CancelRejected>(event)) ++rejections;
            if (record) write_event_record(output, event);
        }
    }
    const auto end = Clock::now();
    total.wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    total.process_ns = latency(samples);
    total.record_bytes = sink.bytes();
    total.record_hash = sink.hash();
    const auto events_per_cycle = scenario == Scenario::Sweep ? 16ULL : scenario == Scenario::Match ? 6ULL : 3ULL;
    const auto fills = scenario == Scenario::Sweep ? 3ULL : scenario == Scenario::Match ? 1ULL : 0ULL;
    const auto units = scenario == Scenario::Sweep ? 6ULL : scenario == Scenario::Match ? 10ULL : 0ULL;
    const auto notional = scenario == Scenario::Sweep ? 608ULL : scenario == Scenario::Match ? 1000ULL : 0ULL;
    if (total.requests != cycles * width || total.events != cycles * events_per_cycle || total.executions != cycles * fills ||
        total.volume_units != cycles * units || total.traded_tick_units != cycles * notional || rejections != 0 ||
        !output || (record && total.record_bytes == 0) || (!record && total.record_bytes != 0) || total.wall_ns <= 0) {
        throw std::runtime_error{"routed benchmark totals or clock check failed"};
    }
    for (std::size_t index = 0; index < result.loads.size(); ++index) {
        const auto& load = result.loads[index];
        const auto& owner = *exchange.find({index + 1});
        const auto time = load.cycles == 0 ? 0 : (load.last_cycle + 1) * width - 1;
        const auto last = owner.last_trade();
        const bool last_valid = load.cycles == 0 || scenario == Scenario::Cancel ? !last : last &&
            last->instrument_id == InstrumentId{index + 1} && last->aggressive_side == Side::Buy &&
            last->aggressive_order_id.value == (load.last_cycle + 1) * width &&
            last->resting_order_id.value == (load.last_cycle + 1) * width - 1 &&
            last->buyer_id == ParticipantId{2} && last->seller_id == ParticipantId{1} &&
            last->price.ticks() == (scenario == Scenario::Match ? 100 : 102) &&
            last->quantity.units() == (scenario == Scenario::Match ? 10 : 3) && last->timestamp == LogicalTime{time} &&
            last->request_sequence.value == load.cycles * width && last->event_sequence.value == load.cycles * events_per_cycle - 2;
        if (!owner.book().empty() || !owner.book().check_invariants() || !last_valid ||
            owner.current_time() != LogicalTime{time} || owner.last_request_sequence().value != load.cycles * width ||
            owner.last_event_sequence().value != load.cycles * events_per_cycle ||
            to_string(owner.cumulative_volume()) != std::to_string(load.cycles * units)) {
            throw std::runtime_error{"routed benchmark per-instrument state check failed"};
        }
    }
    return result;
}

} // namespace marketlab::benchmark
