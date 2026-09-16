# ACRE2 fork — ambient battle sound

Public fork of [IDI-Systems/acre2](https://github.com/IDI-Systems/acre2)
(`ljackson330/acre2`) — so GitHub Actions is free here, on standard runners.
`origin` is the fork; `upstream` is IDI-Systems, fetch-only.

Feature work lives in [ambient/](ambient/). [gameplan.md](gameplan.md) is the
design and the constraints that govern it; **[ambient/README.md](ambient/README.md)
is the living document** — current state, the measurements everything is tuned
from, the gotchas, and what is still unverified. Read the README first, and
treat it as authoritative wherever the two disagree.

## Environment

This is a **Linux** development and play setup, which the upstream project does
not support. Both Arma and TeamSpeak run under **Proton 10 (wine-10.0) inside
Arma's own prefix**, launched by `~/Arma3Helper.sh` (from
[armaonlinux](https://github.com/ninelore/armaonlinux)). System Wine is not
involved — check Proton's binaries, not `/usr/lib/wine`, when reasoning about
runtime behaviour.

Proton does **not** implement `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
(no `VirtualAudioDevice_Process_Loopback` in its `mmdevapi.dll`), so the
Windows process-loopback design in gameplan.md cannot work here. The Linux
backend captures Arma's PipeWire node instead, via a native helper that feeds
the plugin over a loopback socket.

## Build

```
./ambient/build-mingw.sh            # build acre2_win64.dll
./ambient/build-mingw.sh --install  # build and install
./ambient/build-mingw.sh --status   # which build is live where
```

Cross-compiles with mingw-w64; no MSVC or Windows machine needed. The script
self-verifies (31 `ts3plugin_*` exports, no mingw runtime DLL dependencies).
Upstream's MSVC path is preserved — every workaround is behind `if(MSVC)` or
lives in `ambient/compat/`.

MSVC incompatibilities handled: header case-sensitivity (generated symlink
farm), the MSVC Concurrency Runtime containers, `x3daudio.h` missing constants
and `extern "C"` guards, `__int16`/`__int64` keywords, and `api_compat.asm`
(MASM, replaced by `api_compat_gcc.cpp` for non-MSVC).

## Gotchas that cost time

- **`ACRE2Steam` overwrites the TeamSpeak plugin on every Arma launch**, copying
  it from the `@acre2` mod folder whenever the two differ. Installing only to
  TeamSpeak silently reverts to stock next launch. `--install` writes both.
  Steam restoring the Workshop copy has the same effect — check `--status`.
- **`acre2.ini` is read once**, in `CEngine::initialize()` at plugin start.
  Editing it while TeamSpeak runs does nothing. Restart TeamSpeak.
- **The ini parser only treats `;` as a comment after whitespace**, but ACRE2
  writes bare trailing `;`. Harmless for numbers; it becomes part of any string
  value. Write string settings without one.
- **TeamSpeak's Capture → Begin Test is useless for verification.** It mutes the
  microphone and distorts audio. Use `ambientDumpFile` instead, which writes the
  real post-mix capture buffer.
- **Under Proton, Arma and TeamSpeak both report
  `application.process.binary = wine64-preloader`.** Only `application.name`
  distinguishes their PipeWire nodes.
- **`onEditCapturedVoiceDataEvent`'s `edited` is a bitmask.** Bit 1 must be set
  when samples are modified or TeamSpeak discards them silently — it presents
  as broken capture while the mix works perfectly. Bit 2 arrives on input
  meaning "about to be sent". Stock ACRE2 never writes this parameter.
- **The ambient DSP lives in `ambient-helper.py`, so it is Linux-only.** The
  Windows WASAPI backend captures in-process and never touches the helper, which
  means a Windows transmitter gets raw, untuned ambience. Porting the chain to
  C++ is the obvious next code task, and would move the settled values into
  `acre2.ini` rather than helper defaults.
- **Reach for the helper's `--makeup` before `ambientVolume`.** The compressor
  holds the peaks, so makeup lifts the quiet bed without the gunfire coming back.
  Lowering `ambientVolume` pulls everything down equally and takes the bed out
  first — which is the trap the compressor exists to avoid.

- **The first key-up of a TeamSpeak session has no ambience, by design.** The
  backend probe resolves on that transmission; the fallback applies from the
  second onward. Selection deliberately never blocks, because `start()` runs on
  the push-to-talk path. Not a bug — don't chase it.
- **ACRE2 imports `X3DAudio1_7.dll`**, a legacy DirectX SDK redistributable
  that is not part of Windows. Stock upstream imports it too, so anyone running
  ACRE2 has it — but on a clean machine TeamSpeak just says "Failed to load
  plugin" with no hint why.
- **Windows audio is per-session, and SSH lands in session 0.** Anything
  launched over SSH in the VM can neither render nor capture: it activates
  happily and records permanent silence, which looks exactly like broken
  capture. Use `schtasks /it` to run in the interactive session.
- **`Stop-Process -Name powershell` over SSH kills the session's own shell**, so
  everything after it silently never runs. Exclude `$PID`.
- **`AmbientWasapi.{h,cpp}` must stay free of ACRE2 dependencies** (no `LOG`,
  no settings) so `ambient/probe` can build it standalone. That property is what
  makes the Windows capture path testable without TeamSpeak or Arma.

- **`set -o pipefail` plus `grep -q` inverts a check** — grep exits on first
  match, the producer takes SIGPIPE, the pipeline returns non-zero. This
  silently disabled a guard in the build script. Capture output, then match.

## Testing

Needs Arma running and producing audio; most of it cannot be verified from a
terminal alone. Anything touching the audio path should be validated natively
on Linux first where possible.

A Windows 11 VM in `~/VMs`, driven by `./ambient/vm.sh`, exists for the WASAPI
backend — Proton cannot run it and GitHub's runners have no audio endpoint, so
it is the only place that path executes. It is not needed for day-to-day work.

`./ambient/run-tests.sh` builds and runs the ring buffer and gate tests three
ways — optimised, ASan+UBSan, and ThreadSanitizer. They are shared by both
backends, so they are the cheapest protection available for the Windows port;
CI runs them on every push that touches the ambient sources.

`./ambient/build-probe.sh` builds `ambient-probe.exe`, the standalone WASAPI
process-loopback test. It cannot succeed under Proton, which has no
process-loopback device — a clean activation failure there is the correct
result, not a bug.
