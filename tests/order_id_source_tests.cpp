#include <marketlab/order_id_source.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

using marketlab::OrderId;
using marketlab::OrderIdSource;

constexpr bool final_id_is_usable() {
    OrderIdSource ids{{std::numeric_limits<std::uint64_t>::max()}};
    return ids.peek() == OrderId{std::numeric_limits<std::uint64_t>::max()} &&
        ids.next() == OrderId{std::numeric_limits<std::uint64_t>::max()} &&
        !ids.peek() && !ids.next() && !ids.next();
}
static_assert(final_id_is_usable());
static_assert(std::is_nothrow_copy_assignable_v<OrderIdSource>);

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

int main() {
    try {
        bool refused = false;
        try { OrderIdSource invalid{{0}}; } catch (const std::invalid_argument&) { refused = true; }
        require(refused, "zero starting ID accepted");
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        for (const auto first : std::array<std::uint64_t, 3>{1, 1000, maximum - 1000}) {
            OrderIdSource ids{{first}};
            for (std::uint64_t offset = 0; offset <= 1000; ++offset) {
                const auto expected = OrderId{first + offset};
                require(ids.peek() == expected && ids.peek() == expected && ids.next() == expected,
                    "ID sequence or non-consuming peek differs");
            }
            if (first == maximum - 1000) {
                require(!ids.peek() && !ids.next(), "ID sequence wrapped after exhaustion");
            }
        }
        OrderIdSource ids{{42}};
        auto checkpoint = ids;
        require(ids.next() == checkpoint.next() && ids.peek() == checkpoint.peek(), "ID checkpoint differs");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
