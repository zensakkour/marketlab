#include <marketlab/worker_assignment_plan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
std::ptrdiff_t allocations_before_failure = -1;
}

// Preserve both replacement boundaries for GCC allocation-pair diagnostics.
#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void* operator new(std::size_t bytes) {
    if (allocations_before_failure >= 0 && allocations_before_failure-- == 0) throw std::bad_alloc{};
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

static_assert(!std::is_copy_constructible_v<WorkerAssignmentPlan>);
static_assert(!std::is_move_constructible_v<WorkerAssignmentPlan>);
static_assert(std::is_same_v<decltype(std::declval<WorkerAssignmentPlan&>().assignments()),
    std::span<const InstrumentWorkerAssignment>>);
static_assert(std::is_same_v<decltype(std::declval<const WorkerAssignmentPlan&>().worker_for({1})),
    std::optional<std::size_t>>);
static_assert(noexcept(std::declval<const WorkerAssignmentPlan&>().worker_for({1})));
static_assert(noexcept(std::declval<const WorkerAssignmentPlan&>().assignments()));
static_assert(noexcept(std::declval<const WorkerAssignmentPlan&>().worker_count()));

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

auto configurations() {
    const auto tick = TickSize::from_decimal(1, 2).value();
    return std::array{Instrument{{42}, "B", tick}, Instrument{{1}, "A", tick},
        Instrument{{UINT64_MAX}, "C", tick}};
}

constexpr std::array expected{
    InstrumentWorkerAssignment{{1}, 0}, InstrumentWorkerAssignment{{42}, 2},
    InstrumentWorkerAssignment{{UINT64_MAX}, 2}};

void check_plan(const WorkerAssignmentPlan& plan) {
    require(plan.worker_count() == 4 && std::ranges::equal(plan.assignments(), expected),
        "worker plan changed bindings or sorted order");
    for (const auto& assignment : expected) {
        require(plan.worker_for(assignment.instrument_id) == assignment.worker_index,
            "instrument routed to wrong worker");
    }
    require(!plan.worker_for({0}) && !plan.worker_for({99}), "unknown instrument acquired an owner");
}

void test_ownership_and_permutations() {
    std::optional<WorkerAssignmentPlan> owned;
    {
        auto configs = configurations();
        InstrumentRegistry registry{configs};
        auto source = expected;
        owned.emplace(registry, 4, source);
        source.fill(InstrumentWorkerAssignment{{99}, 99});
        configs[0].id = {99};
    }
    check_plan(*owned);
    const auto view = owned->assignments();
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    WorkerAssignmentPlan independent{registry, 4, view};
    check_plan(independent);
    require(independent.assignments().data() != view.data(), "plans share caller storage");
    auto permutation = expected;
    std::size_t permutations = 0;
    do {
        WorkerAssignmentPlan plan{registry, 4, permutation};
        check_plan(plan);
        ++permutations;
    } while (std::next_permutation(permutation.begin(), permutation.end(),
        [](const auto& left, const auto& right) { return left.instrument_id < right.instrument_id; }));
    require(permutations == 6 && owned->assignments().data() == view.data(),
        "permutation coverage or stable borrowed view changed");
}

void test_invalid_configuration() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    for (int scenario = 0; scenario < 9; ++scenario) {
        auto source = expected;
        std::size_t workers = 4;
        std::span<const InstrumentWorkerAssignment> bindings{source};
        if (scenario == 0) workers = 0;
        if (scenario == 1) bindings = bindings.first(2);
        if (scenario == 2) source[1].instrument_id = {99};
        if (scenario == 3) source[1].instrument_id = {0};
        if (scenario == 4) source[1] = source[0];
        if (scenario == 5) source[1].instrument_id = source[0].instrument_id;
        if (scenario == 6) source[1].worker_index = workers;
        if (scenario == 7) source[1].worker_index = std::numeric_limits<std::size_t>::max();
        if (scenario == 8) bindings = {};
        const auto before = source;
        bool refused = false;
        try { WorkerAssignmentPlan invalid{registry, workers, bindings}; }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused && source == before && registry.size() == 3 && registry.find({42}),
            "invalid worker plan accepted or changed its inputs");
    }
    const std::array extra{expected[0], expected[1], expected[2], expected[0]};
    bool refused = false;
    try { WorkerAssignmentPlan invalid{registry, 4, extra}; }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "extra instrument assignment accepted");
}

void test_empty_and_index_boundaries() {
    InstrumentRegistry empty;
    WorkerAssignmentPlan no_instruments{empty, 1, {}};
    require(no_instruments.worker_count() == 1 && no_instruments.assignments().empty() &&
        !no_instruments.worker_for({1}), "empty registry cannot use an idle worker");
    bool refused = false;
    try { WorkerAssignmentPlan invalid{empty, 0, {}}; }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "empty registry bypassed positive worker count");
    const auto configs = configurations();
    InstrumentRegistry registry{configs};
    auto source = expected;
    for (auto& assignment : source) assignment.worker_index = 0;
    WorkerAssignmentPlan single{registry, 1, source};
    for (const auto& assignment : source) {
        require(single.worker_for(assignment.instrument_id) == 0, "single-worker mapping differs");
    }
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    source[2].worker_index = maximum - 1;
    WorkerAssignmentPlan sparse{registry, maximum, source};
    require(sparse.worker_count() == maximum && sparse.worker_for({UINT64_MAX}) == maximum - 1,
        "largest valid worker index or sparse worker metadata differs");
}

void test_allocation_failure_and_reads() {
    const auto configs = configurations();
    InstrumentRegistry registry{configs}, empty;
    const auto* configuration = registry.find({1});
    const auto source = expected;
    std::size_t failures = 0;
    for (std::ptrdiff_t fault = 0; ; ++fault) {
        bool failed = false;
        allocations_before_failure = fault;
        try {
            WorkerAssignmentPlan plan{registry, 4, source};
            allocations_before_failure = -1;
            check_plan(plan);
        } catch (const std::bad_alloc&) {
            allocations_before_failure = -1;
            failed = true;
            ++failures;
        }
        require(source == expected && registry.find({1}) == configuration && registry.size() == 3,
            "failed plan construction changed source or registry");
        WorkerAssignmentPlan retry{registry, 4, source};
        check_plan(retry);
        if (!failed) break;
    }
    require(failures != 0, "plan construction allocation failures not exercised");
    WorkerAssignmentPlan plan{registry, 4, source};
    const auto* borrowed = plan.assignments().data();
    allocations_before_failure = 0;
    WorkerAssignmentPlan no_instruments{empty, 10, {}};
    const auto count = plan.worker_count();
    const auto view = plan.assignments();
    const auto first = plan.worker_for({1});
    const auto last = plan.worker_for({UINT64_MAX});
    const auto missing = plan.worker_for({99});
    allocations_before_failure = -1;
    require(count == 4 && view.data() == borrowed && first == 0 && last == 2 && !missing &&
        no_instruments.assignments().empty(), "plan lookup or empty construction allocated");
}

int main() {
    try {
        test_ownership_and_permutations();
        test_invalid_configuration();
        test_empty_and_index_boundaries();
        test_allocation_failure_and_reads();
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
