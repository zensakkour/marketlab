#include "partitioned_market_scenario.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>

int main(int argc, char** argv) {
    std::size_t workers = 2;
    bool valid = argc >= 1 && argc <= 4;
    if (valid && argc >= 2) {
        const std::string_view text{argv[1]};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), workers);
        valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && workers >= 1 && workers <= 4;
    }
    bool events = false, threaded = false;
    for (int index = 2; valid && index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--events" && !events) events = true;
        else if (option == "--threaded" && !threaded) threaded = true;
        else valid = false;
    }
    if (!valid) {
        std::cerr << "usage: partitioned_market [worker-count:1..4 [--threaded] [--events]]\n";
        return 2;
    }
    try {
        const auto mode = threaded ? partitioned_example::ExecutionMode::Threaded :
                                     partitioned_example::ExecutionMode::CallerDriven;
        const auto result = partitioned_example::run(workers, mode);
        if (events) partitioned_example::write_events(std::cout, result);
        else partitioned_example::write_summary(std::cout, result);
        std::cout.flush();
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
