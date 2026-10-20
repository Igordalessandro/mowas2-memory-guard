#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "$0")/.." && pwd)
test_build_dir=$(mktemp -d /tmp/mowas2-native-tests.XXXXXX)
trap 'rm -f -- "$test_build_dir/recovery-test" "$test_build_dir/report-test" "$test_build_dir/retry-test"; rmdir -- "$test_build_dir"' EXIT

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
echo "All native regression tests passed."
