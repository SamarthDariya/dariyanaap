#!/usr/bin/env bash
#
# One command to check a chunk. Run from anywhere:
#
#     ./scripts/check.sh          build + test          (the inner loop)
#     ./scripts/check.sh --all    also ASan/UBSan+TSan  (before calling a chunk done)
#
# Build directories are gitignored and persist, so the fast path is a rebuild
# of whatever changed, not a configure.
set -euo pipefail

cd "$(dirname "$0")/.."

# DESIGN.md decision 12: steady_clock only. Checked here rather than reviewed,
# with comment lines dropped so clock.hpp's explanation doesn't match itself.
if grep -rn 'system_clock' src/ | grep -v '//'; then
    echo "FAIL: system_clock is banned in src/ — see src/core/clock.hpp" >&2
    exit 1
fi

run_suite() {
    local dir="$1" label="$2"
    shift 2
    cmake -B "$dir" "$@" >/dev/null

    # Build output is captured rather than discarded: an earlier version sent it
    # to /dev/null and a -Wunqualified-std-cast-call warning survived two
    # chunks unnoticed. Warnings are the gate, not advice.
    local log="$dir/.check-build.log"
    if ! cmake --build "$dir" -j >"$log" 2>&1; then
        cat "$log" >&2
        echo "FAIL: $label build failed" >&2
        exit 1
    fi
    if grep -E 'warning:' "$log"; then
        echo "FAIL: $label built with warnings" >&2
        exit 1
    fi

    echo "--- $label ---"
    ctest --test-dir "$dir" --output-on-failure
}

run_suite build "plain"

if [[ "${1:-}" == "--all" ]]; then
    run_suite build-asan "ASan/UBSan" -DDARIYANAAP_ASAN=ON
    run_suite build-tsan "TSan"       -DDARIYANAAP_TSAN=ON
fi
