#!/usr/bin/env bash
# Build and run the ambient pipeline tests natively on Linux.
#
# The ring buffer and gate are shared by both backends and free of Windows
# dependencies, so they can be tested with real sanitizers on the host instead
# of through Wine. ThreadSanitizer is the point of this script: the ring buffer
# is lock-free single-producer/single-consumer, and a race in it would show up
# as intermittent audio corruption on someone else's machine.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/extensions/src/ACRE2Core"
TEST="$REPO/ambient/tests/test_ambient.cpp"
BUILD="${BUILD_DIR:-$REPO/build-mingw}/tests"

mkdir -p "$BUILD"

COMMON=(-std=c++17 -Wall -Wextra -g -I "$SRC" "$TEST" -pthread)
failed=0

run_variant() {
    local name="$1"; shift
    local bin="$BUILD/test_$name"

    printf '\n=== %s ===\n' "$name"
    if ! g++ "${COMMON[@]}" "$@" -o "$bin"; then
        echo "BUILD FAILED ($name)" >&2
        failed=1
        return
    fi
    if ! "$bin"; then
        echo "TESTS FAILED ($name)" >&2
        failed=1
    fi
}

# Plain -O2: catches anything the optimiser exposes that -O0 hides.
run_variant optimised -O2

# Memory and undefined behaviour.
run_variant asan -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

# Races. Must be built separately -- TSan and ASan cannot be combined.
run_variant tsan -O1 -fsanitize=thread -fno-omit-frame-pointer

echo
if [[ $failed -eq 0 ]]; then
    echo "all variants passed"
else
    echo "one or more variants failed" >&2
    exit 1
fi
