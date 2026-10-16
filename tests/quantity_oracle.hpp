#pragma once

#include <cstdint>
#include <string>
#include <utility>

// Independent decimal arithmetic for checking binary market-data aggregates.
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
