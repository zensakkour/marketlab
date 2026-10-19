#include "partitioned_test_support.hpp"

#include <marketlab/bounded_ingress_queue.hpp>
#include <marketlab/participant_pnl.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <span>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace multi_test;

// Test-owned consumers compose existing APIs; no live runner policy is added.
struct Consumer {
    Consumer(std::span<const Instrument> instruments, std::size_t capacity)
        : ingress{capacity}, exchange{InstrumentRegistry{instruments}, participants} {}

    BoundedIngressQueue ingress;
    MultiInstrumentExchange exchange;
    std::vector<WorkerStep> steps;
    std::exception_ptr failure;
};

void compare_exchange_delivery(std::uint64_t seed, std::size_t symbols, std::size_t workers, std::size_t capacity) {
    const auto configs = workload_instruments(symbols);
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, workers, worker_bindings(registry, workers)};
    PartitionedSimulation reference{registry, plan, participants};
    auto requests = instrument_workload(seed, symbols, 1200);
    for (const auto& request : requests) {
        require(reference.schedule(request).has_value(), "reference scheduling failed");
    }
    // The transport accepts an ordered stream; the existing heap independently
    // derives the same delivery order from the original unsorted input.
    std::stable_sort(requests.begin(), requests.end(), [](const auto& left, const auto& right) {
        const auto time = [](const auto& request) {
            return std::visit([](const auto& value) { return value.timestamp; }, request);
        };
        return time(left) < time(right);
    });
    std::map<std::size_t, std::vector<Instrument>> groups;
    for (const auto& config : configs) groups[*plan.worker_for(config.id)].push_back(config);
    std::map<std::size_t, Consumer> consumers;
    std::map<std::size_t, std::size_t> submitted;
    for (const auto& [index, instruments] : groups) {
        auto& consumer = consumers.try_emplace(index, instruments, capacity).first->second;
        consumer.steps.reserve(requests.size());
        submitted[index] = 0;
    }
    std::atomic<bool> failed{false};
    std::vector<std::jthread> threads;
    threads.reserve(consumers.size());
    for (auto& [index, consumer] : consumers) {
        threads.emplace_back([&, output = &consumer, worker_index = index](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    auto arrival = output->ingress.try_pop();
                    if (!arrival) {
                        if (arrival.error() == IngressPopError::Closed) return;
                        std::this_thread::yield();
                        continue;
                    }
                    auto events = std::visit([&](const auto& request) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(request)>, OrderRequest>) {
                            return output->exchange.process_order(request);
                        } else return output->exchange.process_cancel(request);
                    }, arrival->request);
                    require(events.has_value(), "test consumer exchange failed");
                    output->steps.push_back(WorkerStep{worker_index, std::move(*arrival), std::move(*events)});
                }
            } catch (...) {
                output->failure = std::current_exception();
                failed.store(true);
            }
        });
    }
    for (const auto& request : requests) {
        const auto id = std::visit([](const auto& value) { return value.instrument_id; }, request);
        const auto index = *plan.worker_for(id);
        auto& consumer = consumers.at(index);
        while (!failed.load()) {
            const auto accepted = consumer.ingress.try_push(request);
            if (accepted) {
                require(accepted->value == ++submitted.at(index), "transport assigned a nonlocal sequence");
                break;
            }
            require(accepted.error() == IngressPushError::Full, "ordered transport input was refused");
            std::this_thread::yield();
        }
        if (failed.load()) break;
    }
    for (auto& [index, consumer] : consumers) {
        static_cast<void>(index);
        consumer.ingress.close();
    }
    for (auto& thread : threads) thread.join();
    for (const auto& [index, consumer] : consumers) {
        static_cast<void>(index);
        if (consumer.failure) std::rethrow_exception(consumer.failure);
    }

    std::map<InstrumentId, ParticipantLedger> actual_accounts, expected_accounts;
    for (const auto& config : configs) {
        actual_accounts.try_emplace(config.id, config.id, participants);
        expected_accounts.try_emplace(config.id, config.id, participants);
    }
    std::size_t total = 0;
    for (const auto& [index, consumer] : consumers) {
        require(consumer.ingress.is_closed() && consumer.ingress.size() == 0 &&
            consumer.steps.size() == submitted.at(index), "closed transport lost accepted requests");
        for (std::size_t position = 0; position < consumer.steps.size(); ++position) {
            const auto& step = consumer.steps[position];
            const auto expected = reference.step(index);
            require(expected && *expected && step.worker_index == index && step.arrival.sequence.value == position + 1 &&
                scheduled_time(step.arrival) == scheduled_time((**expected).arrival) &&
                event_text(step.events) == event_text((**expected).events), "transport changed exchange delivery or events");
            const auto id = std::visit([](const auto& value) { return value.instrument_id; }, step.arrival.request);
            require(plan.worker_for(id) == index && actual_accounts.at(id).apply(executions_in(step.events)).has_value() &&
                expected_accounts.at(id).apply(executions_in((**expected).events)).has_value(), "transport accounting refused");
            ++total;
        }
        const auto drained = reference.step(index);
        require(drained && !*drained, "reference retained requests after transport drain");
    }
    require(total == requests.size(), "transport lost or duplicated a submitted request");
    const auto mark = Price::from_ticks(100).value();
    for (const auto& config : configs) {
        const auto* actual = consumers.at(*plan.worker_for(config.id)).exchange.find(config.id);
        require(state(*actual, 64) == state(*reference.find(config.id), 64) && actual->book().check_invariants(),
            "bounded transport changed final book, clocks, counters or market data");
        for (const auto participant : participants) {
            require(actual_accounts.at(config.id).account(participant) == expected_accounts.at(config.id).account(participant) &&
                fifo_pnl(actual_accounts.at(config.id), participant, mark) ==
                    fifo_pnl(expected_accounts.at(config.id), participant, mark), "bounded transport changed cash, position or FIFO PnL");
        }
    }
}

int main() {
    try {
        for (const auto seed : {0ULL, 42ULL}) {
            for (const auto symbols : {1U, 12U}) {
                for (const auto workers : {1U, 4U, 7U}) {
                    for (const auto capacity : {1U, 7U, 64U}) {
                        try { compare_exchange_delivery(seed, symbols, workers, capacity); }
                        catch (...) {
                            std::cerr << "seed=" << seed << " symbols=" << symbols
                                      << " workers=" << workers << " capacity=" << capacity << '\n';
                            throw;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
