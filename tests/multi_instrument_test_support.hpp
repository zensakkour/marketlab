#pragma once

#include <marketlab/event_record.hpp>
#include <marketlab/market_data.hpp>
#include <marketlab/multi_instrument_exchange.hpp>
#include <marketlab/participant_ledger.hpp>
#include <marketlab/simulation_event.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace multi_test {

using namespace marketlab;
inline constexpr std::array participants{ParticipantId{1}, ParticipantId{2}};

inline std::array<Instrument, 2> configurations() {
    return {Instrument{{2}, "BETA", TickSize::from_decimal(5, 1).value()},
        Instrument{{1}, "ALPHA", TickSize::from_decimal(1, 2).value()}};
}

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

inline OrderRequest order(std::uint64_t instrument, std::uint64_t id, std::uint64_t owner,
    Side side, std::int64_t price, std::int64_t units, std::uint64_t time) {
    return {{time}, {instrument}, {owner}, {id}, side, OrderType::Limit, units, price};
}

inline CancelRequest cancel(std::uint64_t instrument, std::uint64_t id,
    std::uint64_t owner, std::uint64_t time) {
    return {{time}, {instrument}, {owner}, {id}};
}

inline std::string event_text(std::span<const ExchangeEvent> events) {
    std::ostringstream output;
    for (const auto& event : events) write_event_record(output, event);
    return output.str();
}

// Full observable state for this bounded fixture, whose client IDs are 1..8.
inline std::string state(const Exchange& exchange) {
    std::ostringstream output;
    const auto& config = exchange.instrument();
    output << config.id.value << ' ' << config.symbol << ' ' << config.tick_size.coefficient()
           << ' ' << static_cast<unsigned>(config.tick_size.scale()) << ' '
           << exchange.current_time().nanoseconds << ' ' << exchange.last_request_sequence().value
           << ' ' << exchange.last_event_sequence().value << ' ' << to_string(exchange.cumulative_volume()) << '\n';
    if (const auto last = exchange.last_trade()) write_event_record(output, *last);
    for (std::uint64_t id = 1; id <= 8; ++id) {
        if (const auto* resting = exchange.book().find({id})) {
            const auto& accepted = resting->order;
            output << accepted.order_id.value << ' ' << accepted.participant_id.value << ' '
                   << accepted.instrument_id.value << ' ' << static_cast<unsigned>(accepted.side) << ' '
                   << accepted.original_quantity.units() << ' ' << accepted.limit_price->ticks() << ' '
                   << accepted.accepted_at.nanoseconds << ' ' << accepted.arrival_sequence.value << ' '
                   << resting->remaining_quantity.units() << '\n';
        }
    }
    const auto depth = exchange.book().depth(64);
    for (const auto* levels : {&depth.bids, &depth.asks}) {
        output << "side\n";
        for (const auto& level : *levels) {
            output << level.price.ticks() << ' ' << to_string(level.quantity) << ' ' << level.order_count << '\n';
        }
    }
    return output.str();
}

} // namespace multi_test
