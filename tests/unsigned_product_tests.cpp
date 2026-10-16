#include <marketlab/aggregate_quantity.hpp>
#include <marketlab/detail/unsigned_product.hpp>

#include "quantity_oracle.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

using marketlab::detail::UnsignedProduct;
using marketlab::detail::multiply_unsigned64;

constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
static_assert(multiply_unsigned64(0, maximum) == UnsignedProduct{0, 0});
static_assert(multiply_unsigned64(maximum, 1) == UnsignedProduct{0, maximum});
static_assert(multiply_unsigned64(maximum, maximum) == UnsignedProduct{maximum - 1, 1});
static_assert(multiply_unsigned64(maximum, maximum - 1) == UnsignedProduct{maximum - 2, 2});
static_assert(multiply_unsigned64(0xffffffff, 0x100000001) == UnsignedProduct{0, maximum});
static_assert(multiply_unsigned64(0x100000000, 0x100000000) == UnsignedProduct{1, 0});
static_assert(multiply_unsigned64(0x7fffffffffffffff, 0x7fffffffffffffff) == UnsignedProduct{0x3fffffffffffffff, 1});
static_assert(noexcept(multiply_unsigned64(maximum, maximum)));

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

void test_single_bits() {
    for (unsigned int first = 0; first < 64; ++first) {
        for (unsigned int second = 0; second < 64; ++second) {
            const auto bit = first + second;
            const UnsignedProduct expected = bit < 64 ?
                UnsignedProduct{0, std::uint64_t{1} << bit} :
                UnsignedProduct{std::uint64_t{1} << (bit - 64), 0};
            require(multiply_unsigned64(std::uint64_t{1} << first, std::uint64_t{1} << second) == expected,
                "single-bit product differs");
        }
    }
}

void test_decimal_products() {
    std::mt19937_64 random{1729};
    for (int index = 0; index < 10000; ++index) {
        const auto left = random();
        const auto right = random();
        const auto result = multiply_unsigned64(left, right);
        require(result == multiply_unsigned64(right, left), "product depends on operand order");
        require(marketlab::to_string(marketlab::AggregateQuantity{result.high, result.low}) ==
            decimal_product(left, right), "binary product differs from decimal schoolbook arithmetic");
    }
    const auto signed_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto bounded = multiply_unsigned64(signed_max, signed_max);
    require(marketlab::to_string(marketlab::AggregateQuantity{bounded.high, bounded.low}) ==
        "85070591730234615847396907784232501249", "maximum signed-input product differs");
}

int main() {
    try {
        test_single_bits();
        test_decimal_products();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
