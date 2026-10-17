#include "multi_instrument_scenario.hpp"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const bool events = argc == 2 && std::string_view{argv[1]} == "--events";
    if (argc != 1 && !events) {
        std::cerr << "usage: multi_instrument [--events]\n";
        return 2;
    }
    try {
        const auto result = multi_instrument_example::run();
        if (events) multi_instrument_example::write_events(std::cout, result);
        else multi_instrument_example::write_summary(std::cout, result);
        std::cout.flush();
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
