#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
    printf '%s\n' 'ThreadSanitizer validation requires Linux (including WSL).' >&2
    exit 2
fi

CDPATH= cd -- "$(dirname -- "$0")/.."
compiler=${CXX:-g++}
build_dir=build/tsan
mkdir -p "$build_dir"

for test_name in threaded_simulation_tests threaded_simulation_allocation_tests partitioned_market_tests \
                 bounded_ingress_queue_tests bounded_ingress_queue_allocation_tests \
                 bounded_ingress_exchange_tests bounded_ingress_exchange_allocation_tests \
                 streaming_simulation_tests streaming_simulation_allocation_tests; do
    printf 'Building %s with ThreadSanitizer\n' "$test_name"
    "$compiler" -std=c++23 -O1 -g -fsanitize=thread -fno-omit-frame-pointer -pthread \
        -Wall -Wextra -Wpedantic -Werror -Iinclude \
        "tests/$test_name.cpp" -o "$build_dir/$test_name"
    printf 'Running %s\n' "$test_name"
    TSAN_OPTIONS=halt_on_error=1:exitcode=66 "$build_dir/$test_name"
done
