cmake_minimum_required(VERSION 3.20)
project(MarketLab LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(marketlab INTERFACE)
target_include_directories(marketlab INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(marketlab INTERFACE cxx_std_23)

include(CTest)
if(BUILD_TESTING)
    foreach(test_name IN ITEMS price domain requests orders events simulation
                               order_book order_book_allocation order_book_matching
                               order_book_limit)
        add_executable(${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(${test_name}_tests PRIVATE marketlab)
        if(MSVC)
            target_compile_options(${test_name}_tests PRIVATE /W4 /WX)
        else()
            target_compile_options(${test_name}_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name}_tests)
    endforeach()
endif()
