#pragma once

#include <marketlab/instrument.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace marketlab {

// Fixed, owning configuration. No registration changes after construction.
class InstrumentRegistry {
public:
    explicit InstrumentRegistry(std::span<const Instrument> instruments = {}) {
        for (const auto& instrument : instruments) {
            if (!is_valid(instrument)) {
                throw std::invalid_argument{"registry requires valid instruments"};
            }
        }
        instruments_.assign(instruments.begin(), instruments.end());
        std::ranges::sort(instruments_, {}, &Instrument::id);
        const auto duplicate = std::adjacent_find(instruments_.begin(), instruments_.end(),
            [](const Instrument& left, const Instrument& right) { return left.id == right.id; });
        if (duplicate != instruments_.end()) {
            throw std::invalid_argument{"registry requires unique instrument IDs"};
        }
    }

    InstrumentRegistry(const InstrumentRegistry&) = delete;
    InstrumentRegistry& operator=(const InstrumentRegistry&) = delete;

    [[nodiscard]] bool empty() const noexcept { return instruments_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }

    // Borrowed configuration stays valid until registry destruction.
    [[nodiscard]] std::span<const Instrument> instruments() const noexcept { return instruments_; }

    [[nodiscard]] const Instrument* find(InstrumentId id) const noexcept {
        const auto found = std::ranges::lower_bound(instruments_, id, {}, &Instrument::id);
        return found != instruments_.end() && found->id == id ? &*found : nullptr;
    }

private:
    std::vector<Instrument> instruments_;
};

} // namespace marketlab
