#include <marketlab/instrument.hpp>
#include <marketlab/requests.hpp>
#include <marketlab/time.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <type_traits>

using marketlab::CancelRequest;
using marketlab::Instrument;
using marketlab::InstrumentId;
using marketlab::LogicalTime;
using marketlab::OrderId;
using marketlab::OrderRequest;
using marketlab::OrderType;
using marketlab::ParticipantId;
using marketlab::RejectionReason;
using marketlab::Side;
using marketlab::TickSize;
using marketlab::is_valid;
using marketlab::validate_fields;

static_assert(!std::is_default_constructible_v<TickSize>);
static_assert(!std::is_convertible_v<std::uint64_t, LogicalTime>);
static_assert(LogicalTime{}.nanoseconds == 0);
constexpr auto cent = TickSize::from_decimal(1, 2).value();
static_assert(cent.coefficient() == 1 && cent.scale() == 2);
constexpr OrderRequest valid_limit{
    .timestamp = LogicalTime{0},
    .instrument_id = InstrumentId{1},
    .participant_id = ParticipantId{2},
    .order_id = OrderId{3},
    .side = Side::Buy,
    .type = OrderType::Limit,
    .quantity_units = 5,
    .limit_price_ticks = 101,
};
static_assert(!validate_fields(valid_limit));
static_assert(validate_fields(OrderRequest{}) == RejectionReason::InvalidInstrument);

int main() {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    struct TickCase {
        std::int64_t coefficient;
        std::uint32_t scale;
        bool accepted;
    };
    constexpr std::array tick_cases{
        TickCase{std::numeric_limits<std::int64_t>::min(), 0, false},
        TickCase{-1, 2, false},
        TickCase{0, 2, false},
        TickCase{1, 0, true},
        TickCase{5, 2, true},
        TickCase{10, 3, true},
        TickCase{maximum, 18, true},
        TickCase{1, 19, false},
        TickCase{1, 256, false},
        TickCase{1, std::numeric_limits<std::uint32_t>::max(), false},
    };
    for (const auto& test : tick_cases) {
        const auto tick = TickSize::from_decimal(test.coefficient, test.scale);
        if (tick.has_value() != test.accepted ||
            (tick && (tick->coefficient() != test.coefficient || tick->scale() != test.scale))) {
            std::cerr << "unexpected tick configuration result\n";
            return 1;
        }
    }
    if (!is_valid(Instrument{InstrumentId{1}, "XYZ", cent}) ||
        is_valid(Instrument{InstrumentId{0}, "XYZ", cent}) ||
        is_valid(Instrument{InstrumentId{1}, "", cent})) {
        std::cerr << "unexpected instrument validation\n";
        return 1;
    }
    const LogicalTime last{std::numeric_limits<std::uint64_t>::max()};
    if (!(LogicalTime{0} < LogicalTime{1} && LogicalTime{1} < last) ||
        LogicalTime{1} != LogicalTime{1}) {
        std::cerr << "unexpected logical-time comparison\n";
        return 1;
    }

    struct IdCase {
        InstrumentId instrument;
        ParticipantId participant;
        OrderId order;
        std::optional<RejectionReason> expected;
    };
    constexpr auto maximum_id = std::numeric_limits<std::uint64_t>::max();
    constexpr std::array id_cases{
        IdCase{{0}, {0}, {0}, RejectionReason::InvalidInstrument},
        IdCase{{1}, {0}, {0}, RejectionReason::InvalidParticipant},
        IdCase{{1}, {2}, {0}, RejectionReason::InvalidOrderId},
        IdCase{{1}, {2}, {3}, std::nullopt},
        IdCase{{maximum_id}, {maximum_id}, {maximum_id}, std::nullopt},
    };
    for (const auto& test : id_cases) {
        auto order = valid_limit;
        order.instrument_id = test.instrument;
        order.participant_id = test.participant;
        order.order_id = test.order;
        const CancelRequest cancel{last, test.instrument, test.participant, test.order};
        if (validate_fields(order) != test.expected || validate_fields(cancel) != test.expected) {
            std::cerr << "unexpected request-ID validation or precedence\n";
            return 1;
        }
    }

    struct OrderCase {
        const char* name;
        Side side;
        OrderType type;
        std::int64_t quantity;
        std::optional<std::int64_t> price;
        std::optional<RejectionReason> expected;
    };
    constexpr std::array order_cases{
        OrderCase{"buy limit", Side::Buy, OrderType::Limit, 1, 1, std::nullopt},
        OrderCase{"sell limit maxima", Side::Sell, OrderType::Limit, maximum, maximum, std::nullopt},
        OrderCase{"buy market", Side::Buy, OrderType::Market, 5, std::nullopt, std::nullopt},
        OrderCase{"sell market", Side::Sell, OrderType::Market, 5, std::nullopt, std::nullopt},
        OrderCase{"invalid side precedence", static_cast<Side>(0), static_cast<OrderType>(0),
                  0, 0, RejectionReason::InvalidSide},
        OrderCase{"invalid type precedence", Side::Buy, static_cast<OrderType>(0),
                  0, 0, RejectionReason::InvalidOrderType},
        OrderCase{"invalid type maximum", Side::Buy, static_cast<OrderType>(255),
                  5, 101, RejectionReason::InvalidOrderType},
        OrderCase{"quantity signed minimum", Side::Buy, OrderType::Limit,
                  std::numeric_limits<std::int64_t>::min(), 101, RejectionReason::InvalidQuantity},
        OrderCase{"negative quantity", Side::Buy, OrderType::Limit,
                  -1, 101, RejectionReason::InvalidQuantity},
        OrderCase{"quantity before price", Side::Buy, OrderType::Limit,
                  0, 0, RejectionReason::InvalidQuantity},
        OrderCase{"missing limit price", Side::Buy, OrderType::Limit,
                  5, std::nullopt, RejectionReason::InvalidPrice},
        OrderCase{"zero limit price", Side::Buy, OrderType::Limit,
                  5, 0, RejectionReason::InvalidPrice},
        OrderCase{"negative limit price", Side::Buy, OrderType::Limit,
                  5, -1, RejectionReason::InvalidPrice},
        OrderCase{"market carries price", Side::Buy, OrderType::Market,
                  5, 101, RejectionReason::InvalidPrice},
        OrderCase{"market carries zero price", Side::Buy, OrderType::Market,
                  5, 0, RejectionReason::InvalidPrice},
    };
    for (const auto& test : order_cases) {
        auto order = valid_limit;
        order.timestamp = last;
        order.side = test.side;
        order.type = test.type;
        order.quantity_units = test.quantity;
        order.limit_price_ticks = test.price;
        const auto original = order;
        if (validate_fields(order) != test.expected) {
            std::cerr << test.name << ": unexpected field-validation result\n";
            return 1;
        }
        if (order.quantity_units != original.quantity_units ||
            order.limit_price_ticks != original.limit_price_ticks) {
            std::cerr << test.name << ": submitted values changed during validation\n";
            return 1;
        }
    }
}
