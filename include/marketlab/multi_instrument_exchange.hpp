#pragma once

#include <marketlab/exchange.hpp>
#include <marketlab/instrument_registry.hpp>

#include <cstddef>
#include <expected>
#include <map>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace marketlab {

enum class MultiInstrumentError {
    UnknownInstrument,
    RequestSequenceExhausted,
    EventSequenceExhausted,
    OutputCapacityExceeded,
};

namespace detail {
[[nodiscard]] constexpr MultiInstrumentError multi_instrument_error(ExchangeError error) {
    switch (error) {
    case ExchangeError::RequestSequenceExhausted: return MultiInstrumentError::RequestSequenceExhausted;
    case ExchangeError::EventSequenceExhausted: return MultiInstrumentError::EventSequenceExhausted;
    case ExchangeError::OutputCapacityExceeded: return MultiInstrumentError::OutputCapacityExceeded;
    }
    throw std::logic_error{"unknown exchange diagnostic"};
}
} // namespace detail

// One logical writer owns independent exchanges; clocks, counters and order IDs
// are instrument-local. This class provides no global timeline or arrival queue.
class MultiInstrumentExchange {
public:
    using Result = std::expected<std::vector<ExchangeEvent>, MultiInstrumentError>;

    MultiInstrumentExchange(const InstrumentRegistry& instruments, std::span<const ParticipantId> participants) {
        for (const auto participant : participants) {
            if (!is_valid(participant)) {
                throw std::invalid_argument{"multi-instrument exchange requires nonzero participant IDs"};
            }
        }
        for (const auto& instrument : instruments.instruments()) {
            exchanges_.try_emplace(instrument.id, instrument, participants);
        }
    }

    MultiInstrumentExchange(const MultiInstrumentExchange&) = delete;
    MultiInstrumentExchange& operator=(const MultiInstrumentExchange&) = delete;

    [[nodiscard]] bool empty() const noexcept { return exchanges_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return exchanges_.size(); }

    // Borrowed views stay valid until owner destruction; callers cannot mutate a book.
    [[nodiscard]] const Exchange* find(InstrumentId id) const noexcept {
        const auto found = exchanges_.find(id);
        return found == exchanges_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] Result process_order(const OrderRequest& request) {
        const auto found = exchanges_.find(request.instrument_id);
        if (found == exchanges_.end()) return std::unexpected{MultiInstrumentError::UnknownInstrument};
        auto result = found->second.process_order(request);
        if (!result) return std::unexpected{detail::multi_instrument_error(result.error())};
        return std::move(*result);
    }

    [[nodiscard]] Result process_cancel(const CancelRequest& request) {
        const auto found = exchanges_.find(request.instrument_id);
        if (found == exchanges_.end()) return std::unexpected{MultiInstrumentError::UnknownInstrument};
        auto result = found->second.process_cancel(request);
        if (!result) return std::unexpected{detail::multi_instrument_error(result.error())};
        return std::move(*result);
    }

private:
    // The delegated exchange has committed before return; result transfer cannot allocate.
    static_assert(std::is_nothrow_constructible_v<Result, std::vector<ExchangeEvent>&&>);
    std::map<InstrumentId, Exchange> exchanges_;
};

} // namespace marketlab
