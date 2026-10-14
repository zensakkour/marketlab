#pragma once

#include <marketlab/exchange.hpp>

#include <optional>

namespace marketlab {

struct BestPrices {
    InstrumentId instrument_id;
    LogicalTime timestamp;
    RequestSequence last_request_sequence;
    EventSequence last_event_sequence;
    std::optional<Price> bid;
    std::optional<Price> ask;
};

// Value snapshot at a completed request boundary, under the book's single writer.
[[nodiscard]] inline BestPrices best_prices(const Exchange& exchange) noexcept {
    const auto* bid = exchange.book().best_bid();
    const auto* ask = exchange.book().best_ask();
    return {exchange.book().instrument_id(), exchange.current_time(), exchange.last_request_sequence(),
        exchange.last_event_sequence(), bid ? bid->order.limit_price : std::nullopt,
        ask ? ask->order.limit_price : std::nullopt};
}

} // namespace marketlab
