#include "controlled_market_scenario.hpp"

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
        std::cerr << "usage: controlled_market [--events] [seed]\n";
        return 2;
    }
    try {
        const auto result = controlled_market_example::run(seed);
        if (events) controlled_market_example::write_events(std::cout, result);
        else controlled_market_example::write_summary(std::cout, result);
        std::cout.flush();
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
