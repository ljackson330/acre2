#!/usr/bin/env bash
# Cross-compile the ACRE2 TeamSpeak plugin (acre2_win64.dll) on Linux with
# mingw-w64, for use under Proton/Wine.
#
# Upstream builds with MSVC. Three classes of incompatibility are handled here
# without touching upstream sources:
#
#   1. Case-sensitivity. Sources include <Windows.h>, "Mmsystem.h" etc.; mingw
#      ships them lowercase. Windows filesystems are case-insensitive, Linux is
#      not. Generated symlink farm fixes this.
#   2. MSVC Concurrency Runtime (concurrent_queue/unordered_map/unordered_set).
#      Shimmed in compat/ over std:: containers.
#   3. Missing DXSDK constants (X3DAUDIO_PI). Shimmed in compat/.
#
# A handful of genuine source fixes were needed and are committed separately:
# stray token-paste operators in Macros.h that only MSVC tolerates, a backslash
# include path, and a missing <cstdint>.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/extensions"
BUILD="${BUILD_DIR:-$REPO/build-mingw}"
SHIM="$BUILD/caseshim"
COMPAT="$REPO/ambient/compat"
MINGW_INC=/usr/x86_64-w64-mingw32/include

command -v x86_64-w64-mingw32-g++ >/dev/null || { echo "mingw-w64 not installed" >&2; exit 1; }

echo ">> generating case-sensitivity shim"
mkdir -p "$SHIM"
python3 - "$SRC" "$SHIM" "$MINGW_INC" <<'PYEOF'
import os, re, sys
src, shim, inc = sys.argv[1], sys.argv[2], sys.argv[3]

real = {}
for root, _, files in os.walk(inc):
    for f in files:
        rel = os.path.relpath(os.path.join(root, f), inc)
        real.setdefault(rel.lower(), rel)

# Both <system> and "quoted" forms -- ACRE2 uses quotes for some SDK headers.
pat = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
used, local = set(), set()
for root, _, files in os.walk(src):
    for f in files:
        if f.rsplit('.', 1)[-1].lower() in ('c', 'cpp', 'h', 'hpp'):
            local.add(f.lower())
            try:
                used.update(pat.findall(open(os.path.join(root, f), errors='ignore').read()))
            except OSError:
                pass

made = 0
for u in sorted(used):
    if os.path.exists(os.path.join(inc, u)):
        continue                                  # already resolves
    if os.path.basename(u).lower() in local:
        continue                                  # it's a project header
    target = real.get(u.lower())
    if not target:
        continue                                  # not an SDK header at all
    dst = os.path.join(shim, u)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.islink(dst):
        os.symlink(os.path.join(inc, target), dst)
        made += 1
print(f"   {made} new symlink(s)")
PYEOF

# The vendored TS3 SDK headers use the MSVC __int16/__int64 keywords. mingw
# defines those as macros in _mingw.h, but the SDK headers get included before
# anything drags it in, so force-include it ahead of every translation unit.
FLAGS="-isystem $SHIM -isystem $COMPAT -include _mingw.h -include stddef.h"

# Without this the DLL imports libstdc++-6.dll, libgcc_s_seh-1.dll and
# libwinpthread-1.dll, none of which exist in the Proton prefix -- TeamSpeak
# would fail to load the plugin with no diagnostic. (acre_set_linker_options()
# tries to set these, but omits PARENT_SCOPE, so its assignment is a no-op.)
LDFLAGS_STATIC="-static-libgcc -static-libstdc++ -static"

echo ">> configuring"
mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/ambient/mingw-w64-x86_64.cmake" \
    -DUSE_64BIT_BUILD=ON \
    -DCMAKE_CXX_FLAGS="$FLAGS" \
    -DCMAKE_C_FLAGS="$FLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS_STATIC" \
    -DCMAKE_MODULE_LINKER_FLAGS="$LDFLAGS_STATIC" \
    -DDirectX_D3DX11_INCLUDE_DIR="$MINGW_INC" \
    >/dev/null

echo ">> building ACRE2TS"
cmake --build "$BUILD" --target ACRE2TS -j"$(nproc)"

# ACRE2TS/CMakeLists.txt sends the module to extensions/build/win<arch>/.
DLL="$SRC/build/win64/ACRE2TS/libACRE2TS.dll"
[[ -f "$DLL" ]] || { echo "build reported success but $DLL is missing" >&2; exit 1; }

echo
echo "built: $DLL"
echo

# The plugin must not depend on mingw runtime DLLs -- they do not exist in the
# Proton prefix and TeamSpeak would fail to load it with no diagnostic.
if x86_64-w64-mingw32-objdump -p "$DLL" | grep -qE "libstdc\+\+-6|libgcc_s_seh-1|libwinpthread-1"; then
    echo "FAIL: DLL depends on mingw runtime DLLs; static linking did not take" >&2
    exit 1
fi
echo "ok: no mingw runtime dependencies"

echo "ok: $(x86_64-w64-mingw32-objdump -p "$DLL" | grep -oE "ts3plugin_[A-Za-z0-9_]+" | sort -u | wc -l) unique ts3plugin_* exports"

TS3_PLUGINS="$HOME/.steam/steam/steamapps/compatdata/107410/pfx/drive_c/users/steamuser/AppData/Roaming/TS3Client/plugins"
if [[ "${1:-}" == "--install" ]]; then
    [[ -d "$TS3_PLUGINS" ]] || { echo "TS3 plugin dir not found: $TS3_PLUGINS" >&2; exit 1; }
    if [[ -f "$TS3_PLUGINS/acre2_win64.dll" && ! -f "$TS3_PLUGINS/acre2_win64.dll.stock" ]]; then
        cp "$TS3_PLUGINS/acre2_win64.dll" "$TS3_PLUGINS/acre2_win64.dll.stock"
        echo "backed up stock plugin -> acre2_win64.dll.stock"
    fi
    cp "$DLL" "$TS3_PLUGINS/acre2_win64.dll"
    echo "installed to $TS3_PLUGINS/acre2_win64.dll"
    echo "restore with: cp acre2_win64.dll.stock acre2_win64.dll"
else
    echo
    echo "run with --install to copy into the TeamSpeak plugin directory"
fi
