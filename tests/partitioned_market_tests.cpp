#include "../examples/partitioned_market_scenario.hpp"
#include "multi_instrument_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>

using namespace multi_test;

std::map<InstrumentId, std::string> per_instrument_events(const partitioned_example::Result& result) {
    std::map<InstrumentId, std::string> records;
    for (const auto& event : result.events) {
        const auto id = std::visit([](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Execution>) return value.instrument_id;
            else return value.header.instrument_id;
        }, event);
        std::ostringstream output;
        write_event_record(output, event);
        records[id] += output.str();
    }
    return records;
}

void test_example() {
    const std::string report =
        "instrument_id=1 symbol=ALPHA time_ns=12 requests=4 events=9 volume_units=3 bid_ticks=none ask_ticks=none\n"
        "instrument_id=1 participant_id=1 mark_ticks=101 position_units=3 cash_tick_units=-300 total_pnl_tick_units=3\n"
        "instrument_id=1 participant_id=2 mark_ticks=101 position_units=-3 cash_tick_units=300 total_pnl_tick_units=-3\n"
        "instrument_id=2 symbol=BETA time_ns=4 requests=4 events=8 volume_units=5 bid_ticks=none ask_ticks=none\n"
        "instrument_id=2 participant_id=1 mark_ticks=201 position_units=-5 cash_tick_units=1000 total_pnl_tick_units=-5\n"
        "instrument_id=2 participant_id=2 mark_ticks=201 position_units=5 cash_tick_units=-1000 total_pnl_tick_units=5\n"
        "instrument_id=3 symbol=GAMMA time_ns=6 requests=3 events=8 volume_units=3 bid_ticks=none ask_ticks=none\n"
        "instrument_id=3 participant_id=1 mark_ticks=301 position_units=-3 cash_tick_units=900 total_pnl_tick_units=-3\n"
        "instrument_id=3 participant_id=2 mark_ticks=301 position_units=3 cash_tick_units=-900 total_pnl_tick_units=3\n";
    constexpr std::array hashes{0xc18fc1aae8162e95ULL, 0xbe71fb86b4a9b923ULL,
        0xdc9eabc282adc655ULL, 0xdc9eabc282adc655ULL};
    const std::array<std::vector<std::size_t>, 4> counts{{{11}, {7, 4}, {4, 4, 3}, {4, 4, 3, 0}}};
    const auto reference = per_instrument_events(partitioned_example::run(1));
    for (std::size_t workers = 1; workers <= 4; ++workers) {
        auto canonical = partitioned_example::run(workers).events;
        const auto owner = [workers](const ExchangeEvent& event) {
            const auto id = std::visit([](const auto& value) {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Execution>) return value.instrument_id;
                else return value.header.instrument_id;
            }, event);
            return (id.value - 1) % workers;
        };
        // Stable grouping is an independent oracle for joined worker-index publication.
        std::stable_sort(canonical.begin(), canonical.end(), [&](const auto& left, const auto& right) {
            return owner(left) < owner(right);
        });
        for (const auto mode : {partitioned_example::ExecutionMode::CallerDriven,
                               partitioned_example::ExecutionMode::Threaded}) {
            std::string first_events;
            for (int repeat = 0; repeat < 10; ++repeat) {
                const auto result = partitioned_example::run(workers, mode);
                std::ostringstream summary, events, prefix;
                partitioned_example::write_summary(summary, result);
                partitioned_example::write_events(events, result);
                if (mode == partitioned_example::ExecutionMode::Threaded) prefix << "execution=threaded ";
                prefix << "workers=" << workers << " attempts=12 scheduled=11 refused=1 events=25\n";
                for (std::size_t index = 0; index < workers; ++index) {
                    prefix << "worker_index=" << index << " arrivals=" << counts[workers - 1][index] << '\n';
                }
                require(summary.str() == prefix.str() + report && result.worker_steps == counts[workers - 1] &&
                    result.mode == mode && result.attempts == 12 && result.scheduled == 11 && result.refused == 1 &&
                    result.events.size() == 25 && per_instrument_events(result) == reference,
                    "partitioned example report or per-instrument events changed");
                if (repeat == 0) first_events = events.str();
                require(events.str() == first_events, "same mode/partitioning changed publication order");
                if (mode == partitioned_example::ExecutionMode::CallerDriven) {
                    std::uint64_t hash = 14695981039346656037ULL;
                    for (const unsigned char byte : events.str()) hash = (hash ^ byte) * 1099511628211ULL;
                    require(hash == hashes[workers - 1], "caller-driven example event checksum changed");
                } else require(events.str() == event_text(canonical), "threaded export ignored canonical worker order");
                for (const auto& instrument : result.instruments) {
                    require(!instrument.market.prices.bid && !instrument.market.prices.ask &&
                        instrument.accounts[0].cash_flow().magnitude == instrument.accounts[1].cash_flow().magnitude &&
                        instrument.pnl[0].total.magnitude == instrument.pnl[1].total.magnitude,
                        "partitioned example retained a quote or failed accounting reconciliation");
                }
            }
        }
    }
    for (const auto mode : {partitioned_example::ExecutionMode::CallerDriven,
                           partitioned_example::ExecutionMode::Threaded}) {
        for (const auto invalid : {0U, 5U}) {
            bool refused = false;
            try { static_cast<void>(partitioned_example::run(invalid, mode)); }
            catch (const std::invalid_argument&) { refused = true; }
            require(refused, "example worker-count boundary bypassed");
        }
    }
    bool refused = false;
    try { static_cast<void>(partitioned_example::run(2, static_cast<partitioned_example::ExecutionMode>(99))); }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "unknown example execution mode accepted");
}

int main() {
    try { test_example(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
