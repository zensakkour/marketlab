#include "market_making_scenario.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>

int main(int argc, char** argv) {
    const bool events = argc >= 2 && std::string_view{argv[1]} == "--events";
    const int seed_index = events ? 2 : 1;
    std::uint64_t seed = 42;
    bool valid = argc == seed_index || argc == seed_index + 1;
    if (valid && argc == seed_index + 1) {
        const std::string_view text{argv[seed_index]};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seed);
        valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }
    if (!valid) {
        std::cerr << "usage: market_making [--events] [seed]\n";
        return 2;
    }
    try {
        const auto result = market_making_example::run(seed);
        if (events) market_making_example::write_events(std::cout, result);
        else market_making_example::write_summary(std::cout, result);
        std::cout.flush();
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
