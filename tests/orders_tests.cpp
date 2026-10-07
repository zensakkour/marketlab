#include <marketlab/execution.hpp>
#include <marketlab/identifiers.hpp>
#include <marketlab/orders.hpp>
#include <marketlab/price.hpp>
#include <marketlab/quantity.hpp>
#include <marketlab/requests.hpp>
#include <marketlab/side.hpp>
#include <marketlab/time.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <type_traits>

using marketlab::AcceptedOrder;
using marketlab::EventSequence;
using marketlab::Execution;
using marketlab::OrderType;
using marketlab::Price;
using marketlab::Quantity;
using marketlab::RequestSequence;
using marketlab::RestingOrder;
using marketlab::Side;
using marketlab::is_valid;

static_assert(!std::is_convertible_v<RequestSequence, EventSequence>);
static_assert(!std::is_convertible_v<marketlab::OrderId, RequestSequence>);
static_assert(RequestSequence{1} < RequestSequence{2});
static_assert(EventSequence{1} < EventSequence{2});
static_assert(!std::is_default_constructible_v<AcceptedOrder>);
static_assert(!std::is_default_constructible_v<RestingOrder>);
static_assert(!std::is_default_constructible_v<Execution>);
constexpr auto zero = Quantity::from_units(0).value();
constexpr auto five = Quantity::from_units(5).value();
constexpr auto price = Price::from_ticks(101).value();
constexpr AcceptedOrder limit{
    .order_id = {3},
    .participant_id = {2},
    .instrument_id = {1},
    .side = Side::Buy,
    .type = OrderType::Limit,
    .original_quantity = five,
    .limit_price = price,
    .accepted_at = {0},
    .arrival_sequence = {1},
};
constexpr Execution trade{
    .timestamp = {0},
    .event_sequence = {3},
    .request_sequence = {2},
    .instrument_id = {1},
    .aggressive_order_id = {4},
    .resting_order_id = {3},
    .aggressive_side = Side::Sell,
    .buyer_id = {2},
    .seller_id = {5},
    .price = price,
    .quantity = five,
};
static_assert(is_valid(limit));
static_assert(is_valid(RestingOrder{limit, five}));
static_assert(!is_valid(RestingOrder{limit, zero}));
static_assert(is_valid(trade));

int main() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto value : std::array<std::uint64_t, 3>{0, 1, maximum}) {
        if (is_valid(RequestSequence{value}) != (value != 0) ||
            is_valid(EventSequence{value}) != (value != 0)) {
            std::cerr << "unexpected sequence validity\n";
            return 1;
        }
    }

    struct ShapeCase {
        OrderType type;
        std::optional<Price> price;
        Quantity original;
        RequestSequence sequence;
        bool accepted_valid;
        bool resting_valid;
    };
    constexpr auto maximum_units = std::numeric_limits<std::int64_t>::max();
    const auto maximum_quantity = Quantity::from_units(maximum_units).value();
    const auto maximum_price = Price::from_ticks(maximum_units).value();
    const std::array shape_cases{
        ShapeCase{OrderType::Limit, price, five, {1}, true, true},
        ShapeCase{OrderType::Market, std::nullopt, five, {1}, true, false},
        ShapeCase{OrderType::Limit, std::nullopt, five, {1}, false, false},
        ShapeCase{OrderType::Market, price, five, {1}, false, false},
        ShapeCase{static_cast<OrderType>(0), price, five, {1}, false, false},
        ShapeCase{OrderType::Limit, price, zero, {1}, false, false},
        ShapeCase{OrderType::Limit, price, five, {0}, false, false},
        ShapeCase{OrderType::Limit, maximum_price, maximum_quantity, {maximum}, true, true},
    };
    for (const auto& test : shape_cases) {
        auto order = limit;
        order.type = test.type;
        order.limit_price = test.price;
        order.original_quantity = test.original;
        order.arrival_sequence = test.sequence;
        if (is_valid(order) != test.accepted_valid ||
            is_valid(RestingOrder{order, test.original}) != test.resting_valid) {
            std::cerr << "unexpected accepted/resting order shape\n";
            return 1;
        }
    }
    const auto six = Quantity::from_units(6).value();
    if (!is_valid(RestingOrder{limit, Quantity::from_units(3).value()}) ||
        is_valid(RestingOrder{limit, zero}) || is_valid(RestingOrder{limit, six})) {
        std::cerr << "unexpected remaining quantity bounds\n";
        return 1;
    }
    for (int field = 0; field < 4; ++field) {
        auto order = limit;
        switch (field) {
        case 0:
            order.order_id = {};
            break;
        case 1:
            order.participant_id = {};
            break;
        case 2:
            order.instrument_id = {};
            break;
        case 3:
            order.side = static_cast<Side>(0);
            break;
        }
        if (is_valid(order)) {
            std::cerr << "accepted record allowed invalid identity or side\n";
            return 1;
        }
    }

    auto self_match = trade;
    self_match.seller_id = self_match.buyer_id;
    auto buy_execution = trade;
    buy_execution.aggressive_side = Side::Buy;
    auto maximum_trade = trade;
    maximum_trade.price = maximum_price;
    maximum_trade.quantity = maximum_quantity;
    maximum_trade.event_sequence = {maximum};
    maximum_trade.request_sequence = {maximum};
    if (!is_valid(self_match) || !is_valid(buy_execution) || !is_valid(maximum_trade)) {
        std::cerr << "valid execution boundaries or self-match rejected\n";
        return 1;
    }
    for (int field = 0; field < 9; ++field) {
        auto execution = trade;
        switch (field) {
        case 0:
            execution.event_sequence = {};
            break;
        case 1:
            execution.request_sequence = {};
            break;
        case 2:
            execution.instrument_id = {};
            break;
        case 3:
            execution.aggressive_order_id = {};
            break;
        case 4:
            execution.resting_order_id = {};
            break;
        case 5:
            execution.resting_order_id = execution.aggressive_order_id;
            break;
        case 6:
            execution.aggressive_side = static_cast<Side>(0);
            break;
        case 7:
            execution.buyer_id = {};
            break;
        case 8:
            execution.quantity = zero;
            break;
        }
        if (is_valid(execution)) {
            std::cerr << "execution allowed invalid identity, side, or quantity\n";
            return 1;
        }
    }
    auto invalid_seller = trade;
    invalid_seller.seller_id = {};
    if (is_valid(invalid_seller)) {
        std::cerr << "execution allowed invalid seller\n";
        return 1;
    }
}
