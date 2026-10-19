cmake_minimum_required(VERSION 3.20)
project(MarketLab LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_library(marketlab INTERFACE)
target_include_directories(marketlab INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(marketlab INTERFACE cxx_std_23)
target_link_libraries(marketlab INTERFACE Threads::Threads)

option(MARKETLAB_BUILD_BENCHMARKS "Build standalone benchmark executables" OFF)
if(MARKETLAB_BUILD_BENCHMARKS)
    add_executable(exchange_benchmark benchmarks/exchange_benchmark.cpp)
    target_link_libraries(exchange_benchmark PRIVATE marketlab)
    target_compile_definitions(exchange_benchmark PRIVATE
        MARKETLAB_BENCHMARK_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        MARKETLAB_BENCHMARK_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
        MARKETLAB_BENCHMARK_BUILD_CONFIG="$<CONFIG>")
    if(MSVC)
        target_compile_options(exchange_benchmark PRIVATE /W4 /WX)
    else()
        target_compile_options(exchange_benchmark PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

foreach(example_name IN ITEMS controlled_market market_making multi_instrument partitioned_market)
    add_executable(${example_name} examples/${example_name}.cpp)
    target_link_libraries(${example_name} PRIVATE marketlab)
    if(MSVC)
        target_compile_options(${example_name} PRIVATE /W4 /WX)
    else()
        target_compile_options(${example_name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    foreach(test_name IN ITEMS price aggregate_quantity unsigned_product cash_amount
                               domain instrument_registry worker_assignment_plan
                               multi_instrument multi_instrument_allocation multi_instrument_workload
                               partitioned_simulation partitioned_simulation_allocation partitioned_market
                               threaded_simulation threaded_simulation_allocation
                               bounded_ingress_queue bounded_ingress_queue_allocation
                               bounded_ingress_exchange bounded_ingress_exchange_allocation
                               streaming_simulation streaming_simulation_allocation
                               requests orders events
                               event_record market_data time_weighted_spread
                               participant_ledger participant_ledger_allocation
                               participant_pnl participant_fills
                               order_id_source simulation_random scripted_participant
                               participant_plans participant_allocation participant_integration market_making
                               simulation simulation_queue simulation_queue_allocation simulation_delivery
                               order_book order_book_allocation order_book_matching
                               order_book_limit order_book_cancel order_book_market
                               order_book_invariant exchange exchange_scenario exchange_property exchange_benchmark)
        add_executable(${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(${test_name}_tests PRIVATE marketlab)
        if(MSVC)
            target_compile_options(${test_name}_tests PRIVATE /W4 /WX)
        else()
            target_compile_options(${test_name}_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name}_tests)
    endforeach()
    set_tests_properties(threaded_simulation threaded_simulation_allocation partitioned_market
                         bounded_ingress_queue bounded_ingress_queue_allocation
                         bounded_ingress_exchange bounded_ingress_exchange_allocation
                         streaming_simulation streaming_simulation_allocation
                         PROPERTIES TIMEOUT 120)
endif()
