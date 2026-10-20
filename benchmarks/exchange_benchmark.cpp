#include "routed_benchmark.hpp"

#include <charconv>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <system_error>

namespace bench = marketlab::benchmark;

void usage(std::ostream& output) {
    output << "usage: exchange_benchmark <match|cancel|sweep> [cycles:1..1000000] [--record]\n"
        << "       [--book | --instruments N:1..256 [--skewed]]\n";
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") { usage(std::cout); return 0; }
    std::uint64_t cycles = 10000;
    bool valid = argc >= 2 && argc <= 7, seen_cycles = false, record = false, book_only = false, skewed = false;
    std::optional<std::uint64_t> instruments;
    bench::Scenario scenario = bench::Scenario::Match;
    if (valid) {
        const std::string_view text{argv[1]};
        if (text == "cancel") scenario = bench::Scenario::Cancel;
        else if (text == "sweep") scenario = bench::Scenario::Sweep;
        else valid = text == "match";
    }
    for (int index = 2; valid && index < argc; ++index) {
        const std::string_view text{argv[index]};
        if (text == "--record" && !record) record = true;
        else if (text == "--book" && !book_only) book_only = true;
        else if (text == "--skewed" && !skewed) skewed = true;
        else if (text == "--instruments" && !instruments && index + 1 < argc) {
            const std::string_view count{argv[++index]};
            std::uint64_t value{};
            const auto parsed = std::from_chars(count.data(), count.data() + count.size(), value);
            valid = parsed.ec == std::errc{} && parsed.ptr == count.data() + count.size() &&
                value > 0 && value <= bench::maximum_instruments;
            instruments = value;
        }
        else if (!seen_cycles) {
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), cycles);
            valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && cycles > 0 && cycles <= bench::maximum_cycles;
            seen_cycles = true;
        } else valid = false;
    }
    if (!valid || (book_only && instruments) || (skewed && (!instruments || *instruments < 2))) {
        usage(std::cerr); return 2;
    }
    try {
        const auto traffic = skewed ? bench::Traffic::Hot : bench::Traffic::Balanced;
        const auto routed = instruments ? std::optional{bench::run_routed(scenario, cycles, record, *instruments, traffic)} : std::nullopt;
        const auto result = routed ? routed->total : bench::run(scenario, cycles, record, book_only);
        const auto rate = [&](std::uint64_t count) { return static_cast<double>(count) * 1e9 / static_cast<double>(result.wall_ns); };
        std::cout << std::setprecision(17)
            << "{\"schema\":\"marketlab.exchange-benchmark.v1\",\"scenario\":" << std::quoted(bench::name(scenario))
            << ",\"cycles\":" << cycles << ",\"record\":" << (record ? "true" : "false")
            << ",\"engine\":" << std::quoted(routed ? "routed" : book_only ? "book" : "exchange")
            << ",\"traffic\":" << std::quoted(routed ? skewed ? "hot" : "balanced" : "single")
            << ",\"compiler_id\":" << std::quoted(MARKETLAB_BENCHMARK_COMPILER_ID)
            << ",\"compiler_version\":" << std::quoted(MARKETLAB_BENCHMARK_COMPILER_VERSION)
            << ",\"build_config\":" << std::quoted(MARKETLAB_BENCHMARK_BUILD_CONFIG)
            << ",\"workers\":1,\"instruments\":" << instruments.value_or(1)
            << ",\"requests\":" << result.requests << ",\"events\":" << result.events
            << ",\"executions\":" << result.executions << ",\"volume_units\":" << result.volume_units
            << ",\"traded_tick_units\":" << result.traded_tick_units << ",\"wall_ns\":" << result.wall_ns
            << ",\"requests_per_second\":" << rate(result.requests) << ",\"events_per_second\":" << rate(result.events)
            << ",\"executions_per_second\":" << rate(result.executions)
            << ",\"process_ns\":{\"min\":" << result.process_ns.minimum << ",\"p50\":" << result.process_ns.p50
            << ",\"p95\":" << result.process_ns.p95 << ",\"p99\":" << result.process_ns.p99
            << ",\"p999\":" << result.process_ns.p999 << ",\"max\":" << result.process_ns.maximum << '}'
            << ",\"record_bytes\":" << result.record_bytes << ",\"record_fnv1a64\":";
        if (record) std::cout << '"' << std::hex << std::setw(16) << std::setfill('0') << result.record_hash << std::dec << '"';
        else std::cout << "null";
        if (routed) {
            std::cout << ",\"instrument_cycles\":[";
            for (std::size_t index = 0; index < routed->loads.size(); ++index) {
                if (index != 0) std::cout << ',';
                std::cout << routed->loads[index].cycles;
            }
            std::cout << ']';
        }
        std::cout << ",\"checks_passed\":true}\n";
        std::cout.flush();
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
