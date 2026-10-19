#include <marketlab/bounded_ingress_queue.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
std::ptrdiff_t budget = -1;
}

#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void* operator new(std::size_t bytes) {
    if (budget >= 0 && budget-- == 0) throw std::bad_alloc{};
    if (void* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc{};
}
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { ::operator delete(allocation); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

using namespace marketlab;

void test_construction_failure() {
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        budget = fault;
        bool failed = false;
        try { BoundedIngressQueue queue{17}; }
        catch (const std::bad_alloc&) { failed = true; ++failures; }
        budget = -1;
        BoundedIngressQueue retry{17};
        if (retry.capacity() != 17 || retry.size() != 0 || retry.is_closed()) {
            throw std::runtime_error{"failed construction retained state"};
        }
        if (!failed) break;
    }
    if (failures == 0) throw std::runtime_error{"constructor allocation failure was not exercised"};
}

void test_preallocated_operations() {
    for (const auto capacity : {1U, 3U, 17U}) {
        BoundedIngressQueue queue{capacity};
        bool valid = true;
        budget = 0;
        for (std::uint64_t round = 0; round < 100; ++round) {
            for (std::uint64_t index = 0; index < capacity; ++index) {
                const auto pushed = queue.try_push(CancelRequest{{round + 1}, {0}, {0}, {index}});
                valid = valid && pushed && pushed->value == round * capacity + index + 1;
            }
            valid = valid && queue.size() == capacity && queue.capacity() == capacity && !queue.is_closed() &&
                queue.try_push(CancelRequest{{1000}, {}, {}, {}}).error() == IngressPushError::Full &&
                queue.try_push(CancelRequest{{0}, {}, {}, {}}).error() == IngressPushError::InvalidTimestamp;
            for (std::uint64_t index = 0; index < capacity; ++index) {
                const auto event = queue.try_pop();
                valid = valid && event && event->sequence.value == round * capacity + index + 1 &&
                    std::get<CancelRequest>(event->request).target_order_id.value == index;
            }
            valid = valid && queue.try_pop().error() == IngressPopError::Empty;
        }
        valid = valid && queue.try_push(OrderRequest{.timestamp = {100}}).has_value();
        queue.close();
        queue.close();
        valid = valid && queue.is_closed() && queue.try_push(OrderRequest{}).error() == IngressPushError::Closed &&
            queue.try_pop().has_value() && queue.try_pop().error() == IngressPopError::Closed && queue.size() == 0;
        budget = -1;
        if (!valid) throw std::runtime_error{"preallocated queue operation changed FIFO, state or refusal semantics"};
    }
}

int main() {
    try {
        test_construction_failure();
        test_preallocated_operations();
    } catch (const std::exception& error) {
        budget = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
