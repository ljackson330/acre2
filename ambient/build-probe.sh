#!/usr/bin/env bash
# Cross-compile ambient-probe.exe, the standalone WASAPI process-loopback test.
#
# Builds the same CAmbientWasapiSource the plugin uses, so a result from the
# probe is evidence about the shipping capture path rather than about a
# reimplementation of it.
#
# The probe cannot be run here -- Proton has no process-loopback device behind
# ActivateAudioInterfaceAsync, so it will report a failed activation under Wine,
# which is the correct answer rather than a bug. Run it in the Windows VM
# (ambient/vm.sh) or on a real Windows machine.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/extensions/src"
OUT="${1:-$REPO/build-mingw/ambient-probe.exe}"

command -v x86_64-w64-mingw32-g++ >/dev/null || { echo "mingw-w64 not installed" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"

echo ">> building ambient-probe.exe"
x86_64-w64-mingw32-g++ \
    -std=c++17 -O2 -Wall \
    -o "$OUT" \
    -isystem "$REPO/ambient/compat" \
    -I "$SRC/ACRE2Core" \
    "$REPO/ambient/probe/main.cpp" \
    "$SRC/ACRE2Core/AmbientWasapi.cpp" \
    -static-libgcc -static-libstdc++ -static \
    -lole32 -lmmdevapi -luuid

echo "built: $OUT"

# Same reasoning as build-mingw.sh: a probe that needs mingw runtime DLLs is
# useless as something to hand to a tester, who will not have them.
DEPS=$(x86_64-w64-mingw32-objdump -p "$OUT" | grep "DLL Name:" || true)
if grep -qE "libstdc\+\+-6|libgcc_s_seh-1|libwinpthread-1" <<<"$DEPS"; then
    echo "FAIL: depends on mingw runtime DLLs; it will not run on a clean machine" >&2
    exit 1
fi
echo "ok: no mingw runtime dependencies"
echo
echo "$DEPS" | sed 's/^/  /'
