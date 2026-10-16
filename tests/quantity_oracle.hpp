#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Independent decimal arithmetic for checking market-data and accounting values.
inline std::string add_decimal_units(std::string total, std::uint64_t units) {
    auto position = total.size();
    auto carry = units;
    while (position != 0 || carry != 0) {
        if (position == 0) {
            total.insert(total.begin(), static_cast<char>('0' + carry % 10));
            carry /= 10;
        } else {
            const auto digit = static_cast<std::uint64_t>(total[--position] - '0') + carry % 10;
            total[position] = static_cast<char>('0' + digit % 10);
            carry = carry / 10 + digit / 10;
        }
    }
    return total;
}

inline std::pair<bool, std::string> decimal_difference(std::string left, std::string right) {
    const bool negative = left.size() != right.size() ? left.size() < right.size() : left < right;
    if (negative) left.swap(right);
    auto first = left.size();
    auto second = right.size();
    int borrow = 0;
    while (first != 0) {
        int digit = left[--first] - '0' - borrow;
        if (second != 0) digit -= right[--second] - '0';
        borrow = digit < 0;
        left[first] = static_cast<char>('0' + digit + (borrow ? 10 : 0));
    }
    const auto nonzero = left.find_first_not_of('0');
    if (nonzero == std::string::npos) return {false, "0"};
    left.erase(0, nonzero);
    return {negative, left};
}

inline std::string decimal_product(std::string_view first, std::string_view second) {
    std::vector<unsigned int> digits(first.size() + second.size(), 0);
    for (std::size_t i = 0; i < first.size(); ++i) {
        for (std::size_t j = 0; j < second.size(); ++j) {
            digits[i + j + 1] += static_cast<unsigned int>(first[i] - '0') *
                static_cast<unsigned int>(second[j] - '0');
        }
    }
    for (auto index = digits.size() - 1; index != 0; --index) {
        digits[index - 1] += digits[index] / 10;
        digits[index] %= 10;
    }
    std::string result;
    for (const auto digit : digits) {
        if (digit != 0 || !result.empty()) result.push_back(static_cast<char>('0' + digit));
    }
    return result.empty() ? "0" : result;
}

inline std::string decimal_product(std::uint64_t left, std::uint64_t right) {
    return decimal_product(std::to_string(left), std::to_string(right));
}

inline std::string add_decimal(std::string left, std::string right) {
    if (left.size() < right.size()) left.swap(right);
    auto first = left.size();
    auto second = right.size();
    int carry = 0;
    while (first != 0) {
        int digit = left[--first] - '0' + carry;
        if (second != 0) digit += right[--second] - '0';
        left[first] = static_cast<char>('0' + digit % 10);
        carry = digit / 10;
    }
    if (carry != 0) left.insert(left.begin(), '1');
    return left;
}
