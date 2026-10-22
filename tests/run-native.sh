#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "$0")/.." && pwd)
test_build_dir=$(mktemp -d /tmp/mowas2-native-tests.XXXXXX)
trap 'find "$test_build_dir" -maxdepth 1 -type f -delete; rmdir -- "$test_build_dir"' EXIT

test_cxx=${CXX:-g++}
flags=(-std=c++17 -Wall -Wextra -Wconversion -Werror -pthread -I"$project_root/src")
if [[ ${MOWAS2_TEST_SANITIZERS:-0} == 1 ]]; then
    flags+=(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi

"$test_cxx" "${flags[@]}" "$project_root/tests/memory_guard_recovery_state_test.cpp" -o "$test_build_dir/recovery-test"
"$test_build_dir/recovery-test"
"$test_cxx" "${flags[@]}" "$project_root/tests/compact_report_test.cpp" -o "$test_build_dir/report-test"
"$test_build_dir/report-test"
"$test_cxx" "${flags[@]}" "$project_root/tests/retry_policy_test.cpp" -o "$test_build_dir/retry-test"
"$test_build_dir/retry-test"
"$test_cxx" "${flags[@]}" "$project_root/tests/address_space_stats_test.cpp" -o "$test_build_dir/address-test"
"$test_build_dir/address-test"
"$test_cxx" "${flags[@]}" "$project_root/tests/large_allocation_history_test.cpp" -o "$test_build_dir/large-history-test"
"$test_build_dir/large-history-test"
"$test_cxx" "${flags[@]}" "$project_root/tests/stable_steam_text_test.cpp" -o "$test_build_dir/steam-text-test"
"$test_build_dir/steam-text-test"
"$test_cxx" "${flags[@]}" "$project_root/tests/profiling_policy_test.cpp" -o "$test_build_dir/profiling-policy-test"
"$test_build_dir/profiling-policy-test"
"$test_cxx" "${flags[@]}" "$project_root/tests/watch_diagnostics_test.cpp" -o "$test_build_dir/watch-diagnostics-test"
"$test_build_dir/watch-diagnostics-test"
echo "All native regression tests passed."
