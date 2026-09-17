#!/usr/bin/env bash
#
# Prove the vendoring claim, rather than believing it.
#
#     ./scripts/vendor-smoke-test.sh
#
# README says: "Tests and the CLI only build when dariyanaap is the top-level
# project, so vendoring it adds two static libraries and nothing else." That
# sentence has been true by construction since chunk M0 and never once
# checked. Eleven repos are going to depend on it.
#
# Builds a throwaway parent project that adds this repo as a subdirectory and
# links both halves — load for the driver, fault for the target — then asserts:
#
#   1. it configures and builds
#   2. no test binaries were produced
#   3. no CLI binaries were produced
#   4. -Werror was NOT forced on the parent
#   5. both libraries actually work when called
set -euo pipefail

cd "$(dirname "$0")/.."
REPO="$PWD"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/parent/vendor"
# A copy rather than a real submodule: git submodule add needs a remote and a
# network round trip, and what is under test is the CMake behaviour.
cp -R "$REPO" "$WORK/parent/vendor/dariyanaap"
rm -rf "$WORK/parent/vendor/dariyanaap/build"* "$WORK/parent/vendor/dariyanaap/runs"

cat > "$WORK/parent/CMakeLists.txt" <<'PARENT'
cmake_minimum_required(VERSION 3.20)
project(parent LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A warning the parent is entitled to tolerate. If dariyanaap forces -Werror
# on its includers, this project stops building — which is the failure the
# DARIYANAAP_WERROR default exists to prevent.
add_compile_options(-Wall -Wextra)

add_subdirectory(vendor/dariyanaap)

add_executable(parent_app main.cpp)
target_link_libraries(parent_app PRIVATE dariyanaap::load dariyanaap::fault)
PARENT

cat > "$WORK/parent/main.cpp" <<'MAIN'
#include <cstdio>
#include "core/endpoint.hpp"
#include "fault/knobs.hpp"
#include "load/raw_echo.hpp"
#include "stats/histogram.hpp"

int unused_on_purpose;  // the parent's own warning, which must not be fatal

int main() {
    using namespace dariyanaap;
    // Both halves, actually called: the driver side and the target side.
    const RawEcho protocol(64);
    Histogram histogram;
    histogram.record(Micros(100));

    fault::set_drop_probability(0.0);
    const bool dropped = fault::should_drop();

    printf("%s %zu %llu %d\n", Endpoint::parse("127.0.0.1:9000").str().c_str(),
           protocol.request().size(), (unsigned long long)histogram.count(), dropped ? 1 : 0);
    return 0;
}
MAIN

echo "--- configuring the parent ---"
cmake -S "$WORK/parent" -B "$WORK/build" > "$WORK/configure.log" 2>&1 || {
    cat "$WORK/configure.log"; echo "FAIL: parent did not configure" >&2; exit 1; }

echo "--- building ---"
cmake --build "$WORK/build" -j > "$WORK/build.log" 2>&1 || {
    cat "$WORK/build.log"; echo "FAIL: parent did not build" >&2; exit 1; }

fail=0
check() { if [[ -n "$2" ]]; then echo "  FAIL: $1 — found: $2"; fail=1; else echo "  ok: $1"; fi; }

echo "--- what got built ---"
check "no test binaries" "$(find "$WORK/build" -type f -perm -111 -name '*_tests' | tr '\n' ' ')"
check "no CLI binaries" "$(find "$WORK/build" -type f -perm -111 \
    \( -name 'dariyanaap' -o -name 'dariyanaap-null' -o -name 'hist_verify' \
       -o -name 'fault_cost' \) | tr '\n' ' ')"
check "doctest was not fetched" "$(find "$WORK/build" -maxdepth 3 -type d -name 'doctest-src' | tr '\n' ' ')"
check "-Werror not forced on the parent" "$(grep -o '\-Werror' "$WORK/build/CMakeFiles/parent_app.dir/flags.make" 2>/dev/null | head -1)"

echo "--- and the libraries work ---"
OUT="$("$WORK/build/parent_app")"
if [[ "$OUT" == "127.0.0.1:9000 64 1 0" ]]; then
    echo "  ok: both halves callable — \"$OUT\""
else
    echo "  FAIL: unexpected output \"$OUT\""
    fail=1
fi

echo
[[ $fail -eq 0 ]] && echo "vendoring works" || { echo "vendoring is broken" >&2; exit 1; }
