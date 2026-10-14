#include <marketlab/event_record.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>

using namespace marketlab;

Quantity quantity(std::int64_t units) {
    return Quantity::from_units(units).value();
}

EventHeader header(std::uint64_t sequence) {
    return {{7}, {sequence}, {sequence}, {1}};
}

std::array<ExchangeEvent, 9> samples() {
    const auto price = Price::from_ticks(100).value();
    const OrderRequest malformed{{5}, {0}, {0}, {0}, static_cast<Side>(255), static_cast<OrderType>(255),
        std::numeric_limits<std::int64_t>::min(), 0};
    return {
        OrderAccepted{header(1), {{10}, {2}, {1}, Side::Buy, OrderType::Limit, quantity(5), price, {7}, {1}}},
        Execution{{7}, {2}, {2}, {1}, {11}, {10}, Side::Sell, {2}, {3}, price, quantity(2)},
        OrderPartiallyFilled{header(3), {10}, {2}, quantity(3)},
        OrderFilled{header(4), {11}, {3}, quantity(0)},
        OrderRested{header(5), {10}, {2}, price, quantity(3)},
        MarketRemainderExpired{header(6), {12}, {3}, quantity(4)},
        OrderCancelled{header(7), {10}, {2}, price, quantity(3)},
        OrderRejected{{{7}, {8}, {8}, {0}}, malformed, RejectionReason::InvalidTimestamp},
        CancelRejected{header(9), {{7}, {1}, {3}, {10}}, RejectionReason::NotOwner},
    };
}

bool test_all_events() {
    const std::array<std::string_view, 9> expected{
        "v=1 type=OrderAccepted time_ns=7 event_seq=1 request_seq=1 instrument_id=1 order_id=10 participant_id=2 "
        "side=1 order_type=1 original_units=5 limit_ticks=100\n",
        "v=1 type=Execution time_ns=7 event_seq=2 request_seq=2 instrument_id=1 aggressive_order_id=11 "
        "resting_order_id=10 aggressive_side=2 buyer_id=2 seller_id=3 price_ticks=100 quantity_units=2\n",
        "v=1 type=OrderPartiallyFilled time_ns=7 event_seq=3 request_seq=3 instrument_id=1 "
        "order_id=10 participant_id=2 remaining_units=3\n",
        "v=1 type=OrderFilled time_ns=7 event_seq=4 request_seq=4 instrument_id=1 order_id=11 participant_id=3 remaining_units=0\n",
        "v=1 type=OrderRested time_ns=7 event_seq=5 request_seq=5 instrument_id=1 "
        "order_id=10 participant_id=2 price_ticks=100 remaining_units=3\n",
        "v=1 type=MarketRemainderExpired time_ns=7 event_seq=6 request_seq=6 instrument_id=1 "
        "order_id=12 participant_id=3 remaining_units=4\n",
        "v=1 type=OrderCancelled time_ns=7 event_seq=7 request_seq=7 instrument_id=1 "
        "order_id=10 participant_id=2 price_ticks=100 cancelled_units=3\n",
        "v=1 type=OrderRejected time_ns=7 event_seq=8 request_seq=8 instrument_id=0 submitted_ns=5 participant_id=0 "
        "order_id=0 side=255 order_type=255 quantity_units=-9223372036854775808 limit_ticks=0 reason=InvalidTimestamp\n",
        "v=1 type=CancelRejected time_ns=7 event_seq=9 request_seq=9 instrument_id=1 "
        "submitted_ns=7 participant_id=3 target_order_id=10 reason=NotOwner\n",
    };
    const auto events = samples();
    for (std::size_t index = 0; index < events.size(); ++index) {
        std::ostringstream output;
        write_event_record(output, events[index]);
        if (!output || output.str() != expected[index]) {
            std::cerr << "event record " << index << " differs\n";
            return false;
        }
    }
    auto market = std::get<OrderAccepted>(events.front());
    market.order.type = OrderType::Market;
    market.order.limit_price.reset();
    std::ostringstream market_output;
    write_event_record(market_output, market);
    return market_output.str() == "v=1 type=OrderAccepted time_ns=7 event_seq=1 request_seq=1 instrument_id=1 "
        "order_id=10 participant_id=2 side=1 order_type=2 original_units=5 limit_ticks=none\n";
}

class GroupedNumbers final : public std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

bool test_integer_limits_and_stream_settings() {
    constexpr auto maximum_id = std::numeric_limits<std::uint64_t>::max();
    constexpr auto maximum_units = std::numeric_limits<std::int64_t>::max();
    const Execution event{{maximum_id}, {maximum_id}, {maximum_id}, {maximum_id}, {maximum_id}, {maximum_id - 1},
        Side::Buy, {maximum_id}, {maximum_id}, Price::from_ticks(maximum_units).value(), quantity(maximum_units)};
    std::ostringstream output;
    output.imbue(std::locale{std::locale::classic(), new GroupedNumbers});
    output << std::hex << std::showbase << std::uppercase << std::setfill('*') << std::setw(50);
    const auto flags = output.flags();
    write_event_record(output, event);
    return output.flags() == flags && output.width() == 50 && output.fill() == '*' && output.str() ==
        "v=1 type=Execution time_ns=18446744073709551615 event_seq=18446744073709551615 "
        "request_seq=18446744073709551615 instrument_id=18446744073709551615 "
        "aggressive_order_id=18446744073709551615 resting_order_id=18446744073709551614 "
        "aggressive_side=1 buyer_id=18446744073709551615 seller_id=18446744073709551615 "
        "price_ticks=9223372036854775807 quantity_units=9223372036854775807\n";
}

bool test_rejection_names_and_absent_price() {
    const std::array reasons{RejectionReason::InvalidTimestamp, RejectionReason::InvalidInstrument,
        RejectionReason::InvalidParticipant, RejectionReason::InvalidOrderId, RejectionReason::InvalidSide,
        RejectionReason::InvalidOrderType, RejectionReason::InvalidQuantity, RejectionReason::InvalidPrice,
        RejectionReason::DuplicateOrderId, RejectionReason::NotActive, RejectionReason::NotOwner};
    const std::array<std::string_view, 11> names{"InvalidTimestamp", "InvalidInstrument", "InvalidParticipant",
        "InvalidOrderId", "InvalidSide", "InvalidOrderType", "InvalidQuantity", "InvalidPrice",
        "DuplicateOrderId", "NotActive", "NotOwner"};
    for (std::size_t index = 0; index < reasons.size(); ++index) {
        const auto time = reasons[index] == RejectionReason::InvalidTimestamp ? 5U : 7U;
        std::ostringstream output;
        if (index < 9) {
            const OrderRequest request{{time}, {1}, {2}, {10}, Side::Buy, OrderType::Market, -1, std::nullopt};
            write_event_record(output, OrderRejected{header(1), request, reasons[index]});
            if (output.str().find("quantity_units=-1 limit_ticks=none") == std::string::npos) return false;
        } else {
            write_event_record(output, CancelRejected{header(1), {{time}, {1}, {2}, {10}}, reasons[index]});
        }
        if (!output.str().ends_with(std::string{"reason="} + std::string{names[index]} + '\n')) return false;
    }
    return true;
}

class FailingBuffer final : public std::streambuf {
public:
    explicit FailingBuffer(std::streamsize capacity = 0) : remaining_(capacity) {}
    std::string written;

private:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        const auto accepted = std::min(size, remaining_);
        written.append(data, static_cast<std::size_t>(accepted));
        remaining_ -= accepted;
        return accepted;
    }
    int_type overflow(int_type) override { return traits_type::eof(); }
    std::streamsize remaining_;
};

bool test_invalid_records_and_io_failure() {
    std::ostringstream unchanged;
    unchanged << "prefix";
    try {
        write_event_record(unchanged, OrderFilled{{{0}, {0}, {1}, {1}}, {1}, {1}, quantity(0)});
        return false;
    } catch (const std::invalid_argument&) {
        if (unchanged.str() != "prefix") return false;
    }
    FailingBuffer buffer;
    std::ostream failed{&buffer};
    write_event_record(failed, samples().front());
    if (failed || !buffer.written.empty()) return false;
    FailingBuffer partial_buffer{5};
    std::ostream partial{&partial_buffer};
    write_event_record(partial, samples().front());
    if (partial || partial_buffer.written != "v=1 t") return false;
    std::ostream throwing{&buffer};
    throwing.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        write_event_record(throwing, samples().front());
        return false;
    } catch (const std::ios_base::failure&) {
        return true;
    }
}

int main() {
    if (!test_all_events() || !test_integer_limits_and_stream_settings() ||
        !test_rejection_names_and_absent_price() || !test_invalid_records_and_io_failure()) {
        std::cerr << "event serialization, exact integer representation, or stream error behavior differs\n";
        return 1;
    }
}
