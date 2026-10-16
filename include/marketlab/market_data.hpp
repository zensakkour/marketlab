#pragma once

#include <marketlab/exchange.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace marketlab {

struct MidPrice {
    std::int64_t whole_ticks;
    bool half_tick;
    constexpr bool operator==(const MidPrice&) const noexcept = default;
};

struct BestPrices {
    InstrumentId instrument_id;
    LogicalTime timestamp;
    RequestSequence last_request_sequence;
    EventSequence last_event_sequence;
    std::optional<Price> bid;
    std::optional<Price> ask;

    [[nodiscard]] constexpr std::optional<std::int64_t> spread_ticks() const noexcept {
        if (!bid || !ask) {
            return std::nullopt;
        }
        return ask->ticks() - bid->ticks();
    }

    [[nodiscard]] constexpr std::optional<MidPrice> mid_price() const noexcept {
        if (!bid || !ask) {
            return std::nullopt;
        }
        // Two positive signed 64-bit prices sum within UINT64_MAX; avoid a signed sum.
        const auto sum = static_cast<std::uint64_t>(bid->ticks()) + static_cast<std::uint64_t>(ask->ticks());
        return MidPrice{static_cast<std::int64_t>(sum / 2), sum % 2 != 0};
    }
};

// Value snapshot at a completed request boundary, under the book's single writer.
[[nodiscard]] inline BestPrices best_prices(const Exchange& exchange) noexcept {
    const auto* bid = exchange.book().best_bid();
    const auto* ask = exchange.book().best_ask();
    return {exchange.book().instrument_id(), exchange.current_time(), exchange.last_request_sequence(),
        exchange.last_event_sequence(), bid ? bid->order.limit_price : std::nullopt,
        ask ? ask->order.limit_price : std::nullopt};
}

struct MarketSnapshot {
    BestPrices prices;
    std::optional<Execution> last_trade;
    AggregateQuantity cumulative_volume;
};

[[nodiscard]] inline MarketSnapshot market_snapshot(const Exchange& exchange) noexcept {
    return {best_prices(exchange), exchange.last_trade(), exchange.cumulative_volume()};
}

struct DepthSnapshot {
    MarketSnapshot market;
    BookDepth depth;
};

[[nodiscard]] inline DepthSnapshot depth_snapshot(const Exchange& exchange, std::size_t levels) {
    return {market_snapshot(exchange), exchange.book().depth(levels)};
}

} // namespace marketlab
