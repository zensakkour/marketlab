#include "partitioned_test_support.hpp"
#include "../examples/market_making_scenario.hpp"

#include <marketlab/threaded_simulation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace multi_test;

static_assert(!std::is_copy_constructible_v<ThreadedSimulation>);
static_assert(!std::is_move_constructible_v<ThreadedSimulation>);
static_assert(std::is_same_v<decltype(std::declval<ThreadedSimulation&>().find({1})), const Exchange*>);
static_assert(std::is_same_v<decltype(std::declval<ThreadedSimulation&>().queue(0)), const SimulationQueue*>);
static_assert(noexcept(std::declval<const ThreadedSimulation&>().find({1})));
static_assert(noexcept(std::declval<const ThreadedSimulation&>().queue(0)));

void compare_workload(std::uint64_t seed, std::size_t symbols, std::size_t workers) {
    const auto configs = workload_instruments(symbols);
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan plan{registry, workers, worker_bindings(registry, workers)};
    ThreadedSimulation threaded{registry, plan, participants};
    PartitionedSimulation sequential{registry, plan, participants};
    std::map<InstrumentId, ParticipantLedger> accounts, reference_accounts;
    for (const auto& config : configs) {
        accounts.try_emplace(config.id, config.id, participants);
        reference_accounts.try_emplace(config.id, config.id, participants);
    }
    const auto* borrowed_exchange = threaded.find({1});
    const auto* borrowed_queue = threaded.queue(0);
    for (std::uint64_t epoch = 0; epoch < 2; ++epoch) {
        std::map<InstrumentId, std::uint64_t> origins;
        for (const auto& config : configs) origins[config.id] = threaded.find(config.id)->current_time().nanoseconds;
        auto script = instrument_workload(seed + epoch, symbols, 1200);
        for (auto& request : script) {
            std::visit([&](auto& value) { value.timestamp.nanoseconds += origins.at(value.instrument_id); }, request);
            const auto accepted = threaded.schedule(request), expected = sequential.schedule(request);
            require(accepted && expected && *accepted == *expected, "threaded scheduling changed owner sequence");
        }
        const auto batches = threaded.run();
        std::size_t total = 0;
        std::optional<std::size_t> previous;
        for (const auto& batch : batches) {
            require(batch.succeeded() && !batch.steps.empty() && (!previous || *previous < batch.worker_index),
                "threaded batch failed or publication followed completion order");
            previous = batch.worker_index;
            for (const auto& step : batch.steps) {
                const auto expected = sequential.step(batch.worker_index);
                require(expected && *expected && step.worker_index == batch.worker_index &&
                    step.arrival.sequence == (**expected).arrival.sequence &&
                    scheduled_time(step.arrival) == scheduled_time((**expected).arrival) &&
                    event_text(step.events) == event_text((**expected).events), "threaded step differs from sequential queue");
                const auto id = std::visit([](const auto& value) { return value.instrument_id; }, step.arrival.request);
                require(plan.worker_for(id) == batch.worker_index &&
                    accounts.at(id).apply(executions_in(step.events)).has_value() &&
                    reference_accounts.at(id).apply(executions_in((**expected).events)).has_value(),
                    "threaded step belongs to wrong worker or accounting refused");
                ++total;
            }
            require(sequential.step(batch.worker_index)->has_value() == false &&
                threaded.queue(batch.worker_index)->empty(), "successful worker did not drain its closed batch");
        }
        require(total == script.size() && threaded.run().empty(), "batch lost an arrival or reran committed work");
        const auto mark = Price::from_ticks(100).value();
        for (const auto& config : configs) {
            require(state(*threaded.find(config.id), 64) == state(*sequential.find(config.id), 64) &&
                threaded.find(config.id)->book().check_invariants(), "threaded final state differs");
            for (const auto participant : participants) {
                require(accounts.at(config.id).account(participant) == reference_accounts.at(config.id).account(participant) &&
                    fifo_pnl(accounts.at(config.id), participant, mark) ==
                        fifo_pnl(reference_accounts.at(config.id), participant, mark), "threaded positions/cash/FIFO PnL differ");
            }
        }
        require(threaded.find({1}) == borrowed_exchange && threaded.queue(0) == borrowed_queue,
            "run invalidated a borrowed exchange or queue object");
    }
}

void test_source_lifetime_and_sparse_configuration() {
    std::optional<ThreadedSimulation> owned;
    {
        auto configs = configurations();
        InstrumentRegistry registry{configs};
        auto bindings = worker_bindings(registry, 2);
        WorkerAssignmentPlan plan{registry, 2, bindings};
        auto ids = participants;
        owned.emplace(registry, plan, ids);
        configs[0].symbol = "CHANGED";
        ids.fill(ParticipantId{99});
        bindings[0].worker_index = 99;
    }
    require(owned->find({1})->instrument().symbol == "ALPHA" && !owned->find({0}) && !owned->queue(2) &&
        owned->plan().worker_for({2}) == 1 && owned->run().empty(), "threaded owner borrowed source configuration");
    require(owned->schedule(order(1, 1, 1, Side::Buy, 100, 5, 10))->value == 1 &&
        owned->schedule(order(2, 1, 1, Side::Sell, 200, 5, 1))->value == 1, "queues share an insertion counter");
    const auto result = owned->run();
    require(result.size() == 2 && result[0].succeeded() && result[1].succeeded() &&
        owned->find({1})->current_time() == LogicalTime{10} && owned->find({2})->current_time() == LogicalTime{1},
        "threaded partitions share clocks or fixed registration changed");
    require(owned->schedule(cancel(1, 1, 1, 9)).error() == PartitionedScheduleError::InvalidTimestamp &&
        owned->schedule(cancel(99, 1, 1, 0)).error() == PartitionedScheduleError::UnknownInstrument &&
        owned->queue(0)->last_sequence().value == 1, "refused scheduling consumed a sequence");
    require(owned->schedule(cancel(2, 1, 1, 2))->value == 2, "drained worker cannot resume at local time");
    const auto resumed = owned->run();
    require(resumed.size() == 1 && resumed[0].worker_index == 1 && resumed[0].succeeded() &&
        std::holds_alternative<OrderCancelled>(resumed[0].steps[0].events[0]), "resume reran an idle worker");
    const auto configs = configurations();
    InstrumentRegistry registry{configs}, empty;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const std::array bindings{InstrumentWorkerAssignment{{1}, maximum - 1}, InstrumentWorkerAssignment{{2}, maximum - 2}};
    WorkerAssignmentPlan sparse_plan{registry, maximum, bindings}, empty_plan{empty, maximum, {}};
    ThreadedSimulation sparse{registry, sparse_plan, participants}, idle{empty, empty_plan, participants};
    require(idle.run().empty() && !sparse.queue(0) && sparse.schedule(cancel(1, 1, 1, 0)).has_value(),
        "sparse/idle worker count created dense runtime storage");
    const auto sparse_result = sparse.run();
    require(sparse_result.size() == 1 && sparse_result[0].worker_index == maximum - 1 &&
        sparse_result[0].succeeded(), "maximum active worker index changed");
    for (const auto* source : {&registry, &empty}) {
        const auto& plan = source == &registry ? sparse_plan : empty_plan;
        const std::array invalid_ids{ParticipantId{0}};
        bool refused = false;
        try { ThreadedSimulation invalid{*source, plan, invalid_ids}; }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused, "threaded empty owner bypassed participant validation");
    }
    bool refused = false;
    try { ThreadedSimulation invalid{empty, sparse_plan, participants}; }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "mismatched registry and plan accepted");
}

void test_results_outlive_owner() {
    const auto batches = [] {
        const auto configs = configurations();
        InstrumentRegistry registry{configs};
        WorkerAssignmentPlan plan{registry, 2, worker_bindings(registry, 2)};
        ThreadedSimulation runner{registry, plan, participants};
        for (const auto& request : std::array<SimulationRequest, 4>{
            order(1, 1, 1, Side::Buy, 100, 3, 0), order(1, 2, 2, Side::Sell, 100, 3, 1),
            order(2, 1, 2, Side::Sell, 200, 4, 0), order(2, 2, 1, Side::Buy, 200, 4, 1)}) {
            require(runner.schedule(request).has_value(), "result lifetime fixture scheduling failed");
        }
        return runner.run();
    }(); // The runner, configuration and plan no longer exist.
    require(batches.size() == 2, "owned results lost a partition after owner destruction");
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    for (const auto& batch : batches) {
        require(batch.succeeded() && batch.steps.size() == 2, "owned results lost committed steps");
        const InstrumentId id{batch.worker_index + 1};
        Exchange replay{*registry.find(id), participants};
        ParticipantLedger ledger{id, participants};
        for (const auto& step : batch.steps) {
            const auto expected = isolated_process(replay, step.arrival.request);
            require(expected && event_text(step.events) == event_text(*expected) &&
                ledger.apply(executions_in(step.events)).has_value(), "owned results could not be recorded or accounted after destruction");
        }
        const auto account = ledger.account({1});
        const auto quantity = batch.worker_index == 0 ? 3U : 4U;
        require(account && to_string(account->buy_volume) == std::to_string(quantity) &&
            account->sell_volume == AggregateQuantity{}, "owned executions changed counterparty or quantity");
    }
}

void test_large_uneven_batches() {
    const auto configs = workload_instruments(3);
    InstrumentRegistry registry{configs};
    const std::array bindings{InstrumentWorkerAssignment{{1}, 0},
        InstrumentWorkerAssignment{{2}, 1}, InstrumentWorkerAssignment{{3}, 2}};
    WorkerAssignmentPlan plan{registry, 4, bindings};
    ThreadedSimulation runner{registry, plan, participants};
    constexpr std::array<std::uint64_t, 2> sizes{4096, 64};
    constexpr std::array<std::uint64_t, 2> instruments{1, 3};
    std::map<InstrumentId, Exchange> references;
    for (const auto id : instruments) references.try_emplace(InstrumentId{id}, *registry.find({id}), participants);
    std::vector<WorkerBatch> retained;
    std::array<std::string, 2> retained_records;
    for (std::uint64_t epoch = 0; epoch < 2; ++epoch) {
        const auto time = 10 * (epoch + 1);
        // Schedule the small partition first; publication still starts at worker zero.
        for (std::size_t partition = sizes.size(); partition-- > 0;) {
            const auto count = sizes[partition], id = instruments[partition];
            const auto base = epoch * 2 * count;
            // Descending client IDs distinguish FIFO from accidental ID ordering.
            for (std::uint64_t index = 0; index < count; ++index) {
                const auto scheduled = runner.schedule(order(id, base + count - index, 1, Side::Buy, 100, 1, time));
                require(scheduled && scheduled->value == base + index + 1, "large backlog lost an insertion sequence");
            }
            for (std::uint64_t index = 0; index < count; ++index) {
                const auto scheduled = runner.schedule(order(id, base + count + index + 1, 2, Side::Sell, 100, 1, time));
                require(scheduled && scheduled->value == base + count + index + 1,
                    "large backlog changed local sequence continuity");
            }
            require(runner.queue(partition * 2)->size() == 2 * count,
                "closed backlog lost pending arrivals before run");
        }
        auto batches = runner.run();
        require(batches.size() == 2 && runner.queue(1)->empty() && !runner.queue(3),
            "idle partitions appeared in large-batch publication");
        for (std::size_t partition = 0; partition < sizes.size(); ++partition) {
            const auto count = sizes[partition], id = instruments[partition];
            const auto& batch = batches[partition];
            require(batch.succeeded() && batch.worker_index == partition * 2 && batch.steps.size() == 2 * count,
                "uneven worker batch failed, lost steps or changed publication order");
            auto& replay = references.at({id});
            std::string records;
            for (std::size_t index = 0; index < batch.steps.size(); ++index) {
                const auto& step = batch.steps[index];
                const auto* request = std::get_if<OrderRequest>(&step.arrival.request);
                const auto base = epoch * 2 * count;
                const auto expected_id = base + (index < count ? count - index : index + 1);
                require(request && request->order_id == OrderId{expected_id} &&
                    step.worker_index == batch.worker_index && step.arrival.sequence.value == base + index + 1 &&
                    scheduled_time(step.arrival) == LogicalTime{time}, "equal-time backlog changed FIFO delivery");
                const auto executions = executions_in(step.events);
                require(executions.size() == (index < count ? 0U : 1U), "large batch lost or duplicated a fill");
                if (!executions.empty()) {
                    require(executions[0].resting_order_id == OrderId{base + 2 * count - index} &&
                        executions[0].aggressive_order_id == OrderId{expected_id} &&
                        executions[0].buyer_id == ParticipantId{1} && executions[0].seller_id == ParticipantId{2} &&
                        executions[0].price.ticks() == 100 && executions[0].quantity.units() == 1,
                        "large price level violated FIFO or execution terms");
                }
                const auto expected = isolated_process(replay, step.arrival.request);
                require(expected && event_text(*expected) == event_text(step.events), "large batch differs from isolated exchange");
                if (epoch == 0) records += event_text(step.events);
            }
            const auto* exchange = runner.find({id});
            require(exchange->book().empty() && exchange->book().check_invariants() &&
                exchange->last_request_sequence().value == (epoch + 1) * 2 * count &&
                to_string(exchange->cumulative_volume()) == std::to_string((epoch + 1) * count) &&
                runner.queue(batch.worker_index)->empty(), "large batch final state or drain count differs");
            if (epoch == 0) retained_records[partition] = std::move(records);
        }
        require(runner.find({2})->last_request_sequence().value == 0 && runner.run().empty(),
            "large batch touched an idle instrument or replayed committed arrivals");
        if (epoch == 0) retained = std::move(batches);
    }
    for (std::size_t partition = 0; partition < retained.size(); ++partition) {
        std::string records;
        for (const auto& step : retained[partition].steps) records += event_text(step.events);
        require(records == retained_records[partition], "later run changed previously owned batch results");
    }
}

void test_reference_experiment() {
    constexpr std::array owners{ParticipantId{1}, ParticipantId{2}, ParticipantId{3}, ParticipantId{4}};
    for (const auto seed : {0ULL, 42ULL}) {
        const auto reference = market_making_example::run(seed);
        const std::array configs{Instrument{{1}, "REFERENCE", TickSize::from_decimal(1, 2).value()},
            Instrument{{2}, "OTHER", TickSize::from_decimal(5, 1).value()}};
        InstrumentRegistry registry{configs};
        for (const auto workers : {1U, 2U}) {
            WorkerAssignmentPlan plan{registry, workers, worker_bindings(registry, workers)};
            ThreadedSimulation runner{registry, plan, owners};
            for (const auto& arrival : reference.arrivals) {
                require(runner.schedule(arrival.request).has_value(), "reference arrival scheduling failed");
            }
            for (std::uint64_t index = 1; index <= 20; ++index) {
                require(runner.schedule(order(2, index, 1, Side::Buy, 200, 1, index)).has_value(),
                    "other partition scheduling failed");
            }
            ParticipantLedger ledger{{1}, owners};
            Exchange replay{configs[0], owners};
            TimeWeightedSpread spread;
            std::string actual_events;
            const auto batches = runner.run();
            for (const auto& batch : batches) {
                require(batch.succeeded(), "threaded reference failed");
                for (const auto& step : batch.steps) {
                    const auto id = std::visit([](const auto& value) { return value.instrument_id; }, step.arrival.request);
                    if (id != InstrumentId{1}) continue;
                    actual_events += event_text(step.events);
                    require(ledger.apply(executions_in(step.events)).has_value(), "reference accounting refused");
                    const auto replayed = isolated_process(replay, step.arrival.request);
                    require(replayed && event_text(*replayed) == event_text(step.events) &&
                        spread.observe(scheduled_time(step.arrival), best_prices(replay).spread_ticks()).has_value(),
                        "reference read-only spread replay differs");
                }
            }
            require(actual_events == event_text(reference.events) && state(*runner.find({1}), 10000) == state(replay, 10000) &&
                spread.snapshot(reference.horizon).value() == reference.spread, "threaded reference events/state/spread changed");
            const auto market = depth_snapshot(*runner.find({1}), 64);
            require(market.market.prices.bid == reference.market.market.prices.bid &&
                market.market.prices.ask == reference.market.market.prices.ask &&
                market.market.cumulative_volume == reference.market.market.cumulative_volume &&
                market.depth.bids == reference.market.depth.bids && market.depth.asks == reference.market.depth.asks,
                "reference final market data differs");
            for (const auto& account : reference.participants) {
                require(ledger.account(account.id) == account.account &&
                    fifo_pnl(ledger, account.id, reference.mark) == account.pnl &&
                    participant_fills(ledger, account.id) == account.fills, "threaded reference participant report differs");
            }
        }
    }
}

int main() {
    try {
        test_source_lifetime_and_sparse_configuration();
        for (const auto seed : {0ULL, 42ULL}) {
            for (const auto symbols : {1U, 24U}) {
                for (const auto workers : {1U, 4U, 7U}) compare_workload(seed, symbols, workers);
            }
        }
        test_results_outlive_owner();
        test_large_uneven_batches();
        test_reference_experiment();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
