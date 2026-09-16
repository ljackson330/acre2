#!/usr/bin/env bash
# Build the zip handed to someone testing on real Windows.
#
# Exists so packaging is one reviewed command rather than a sequence remembered
# from last time. It refuses to produce a package from a build containing the
# development split dump, which records the tester's microphone -- that check is
# the whole reason this is a script.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DLL="$REPO/extensions/build/win64/ACRE2TS/libACRE2TS.dll"
OUT="${1:-$HOME/acre2-ambient-test.zip}"
STAGE="$(mktemp -d)/acre2-ambient-test"
trap 'rm -rf "$(dirname "$STAGE")"' EXIT

[[ -f "$DLL" ]] || { echo "no build at $DLL -- run ./ambient/build-mingw.sh first" >&2; exit 1; }

mkdir -p "$STAGE"
cp "$DLL" "$STAGE/acre2_win64.dll"
x86_64-w64-mingw32-strip "$STAGE/acre2_win64.dll"
cp "$REPO/ambient/tester-package/READ-ME-FIRST.txt" "$STAGE/"
"$REPO/ambient/build-probe.sh" "$STAGE/ambient-probe.exe" >/dev/null

# Never ship a build that records someone's microphone. Checked against the
# binary rather than the build flags, because the flags are not what gets sent.
#
# grep -c and a captured variable, never `| grep -q`: under `set -o pipefail`
# grep exits on the first match, strings takes SIGPIPE, and the pipeline returns
# non-zero -- which inverts the test and silently disables the guard. That has
# already happened once in this repo; see CLAUDE.md.
SPLIT_HITS=$(strings -a "$STAGE/acre2_win64.dll" | grep -c "ambientDumpSplitFile" || true)
if [[ "$SPLIT_HITS" -gt 0 ]]; then
    echo "REFUSING: this build contains the development split dump." >&2
    echo "Rebuild without --dev:  ./ambient/build-mingw.sh" >&2
    exit 1
fi

exports=$(x86_64-w64-mingw32-objdump -p "$STAGE/acre2_win64.dll" \
          | grep -oE "ts3plugin_[A-Za-z0-9_]+" | sort -u | wc -l)
[[ "$exports" -eq 31 ]] || { echo "expected 31 ts3plugin_* exports, found $exports" >&2; exit 1; }

rm -f "$OUT"
python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1].removesuffix('.zip'),'zip',sys.argv[2],'acre2-ambient-test')" \
        "$OUT" "$(dirname "$STAGE")"

echo "ok: no development diagnostics, $exports exports"
echo "built: $OUT"
