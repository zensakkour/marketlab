#pragma once

#include <cstdint>
#include <string>

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
