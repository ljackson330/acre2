# ACRE2 fork — ambient battle sound

Public fork of [IDI-Systems/acre2](https://github.com/IDI-Systems/acre2)
(`ljackson330/acre2`) — so GitHub Actions is free here, on standard runners.
`origin` is the fork; `upstream` is IDI-Systems, fetch-only.

Feature work lives in [ambient/](ambient/) and is specified in
[gameplan.md](gameplan.md). **Read [ambient/README.md](ambient/README.md)
first** — it carries the current state, the Phase 0 measurements everything is
tuned from, and what is still unverified.

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
- **`set -o pipefail` plus `grep -q` inverts a check** — grep exits on first
  match, the producer takes SIGPIPE, the pipeline returns non-zero. This
  silently disabled a guard in the build script. Capture output, then match.

## Testing

Needs Arma running and producing audio; most of it cannot be verified from a
terminal alone. Anything touching the audio path should be validated natively
on Linux first where possible.

`./ambient/run-tests.sh` builds and runs the ring buffer and gate tests three
ways — optimised, ASan+UBSan, and ThreadSanitizer. They are shared by both
backends, so they are the cheapest protection available for the Windows port;
CI runs them on every push that touches the ambient sources.

`./ambient/build-probe.sh` builds `ambient-probe.exe`, the standalone WASAPI
process-loopback test. It cannot succeed under Proton, which has no
process-loopback device — a clean activation failure there is the correct
result, not a bug.
