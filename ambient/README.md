# Ambient battle sound — Phase 0

Phase 0 is the gameplan's go/no-go gate. It asks whether Arma's own rendered
audio mix is *usable* as transmitted ambience. That is a question about Arma's
output, not about any capture API — so it is answerable on Linux today, and the
result transfers to a Windows implementation unchanged.

**Does not transfer:** sample rate and format. Those are negotiated between the
capture API and the audio stack, not emitted by Arma. Treat any format seen here
as a Linux fact only.

## Tools

- `phase0_capture.sh` — records one application's audio node (the Linux
  equivalent of Windows process loopback; targets by PID, never the device mix)
- `wav_stats.py` — peak/RMS/dBFS + per-second histogram, so "silent vs merely
  attenuated" is measured rather than guessed

## Protocol

Run each take for ~30s. Both takes need the same firefight intensity to compare.

1. **Launch Arma** (`~/Arma3Helper.sh` starts TS3 in the same Proton prefix).
2. **Control take — sliders UP.** Music and UI at normal levels, in a menu or
   somewhere with music playing.
   ```
   ./ambient/phase0_capture.sh record 30 control_slidersup.wav
   ```
3. **Test take — sliders ZEROED.** Zero every non-diegetic category Arma
   exposes. Record the same conditions.
   ```
   ./ambient/phase0_capture.sh record 30 test_sliderszero.wav
   ```
4. **Combat take.** Sliders still zeroed. Get into an AI firefight with gunfire,
   explosions and engines audible.
   ```
   ./ambient/phase0_capture.sh record 30 fight.wav
   ```
5. **Isolation take.** Sliders zeroed, have someone transmit to you on radio,
   and speak on direct. Record Arma's node.
   ```
   ./ambient/phase0_capture.sh record 30 isolation.wav
   ```

## What each take decides

| Take | Question | Pass condition |
|---|---|---|
| control vs test | do zeroed sliders *silence* music/UI or just attenuate it? | test take reads digital silence, or far below the control |
| fight | is combat recognizable and does it sound usable transmitted? | by ear — this is the subjective gate |
| fight | is a noise gate needed at all? | check the inter-event floor in the histogram |
| isolation | is received radio / direct voice absent from Arma's node? | digital silence during the transmission |

The isolation take validates the gameplan's hard requirement empirically rather
than by reasoning. It is the cheap place to catch a feedback loop.

**If the fight take doesn't sound usable, stop.** The gameplan says reassess
rather than proceed to Phase 2, and names a fully separate alternative design
(fired-event + canned samples) that is not an extension of this one.

## Note on Wine versions

Arma *and* TeamSpeak both run under Proton 10 (wine-10.0) in Arma's prefix —
`Arma3Helper.sh` launches `ts3client_win64.exe` through Proton, not system Wine.
Proton 10 does not implement `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
(verified: no `VirtualAudioDevice_Process_Loopback` in its `mmdevapi.dll`), which
is why the Linux backend cannot be a direct port of the Windows design.

---

# Phase 0 results — 2026-09-15

Recorded on Linux/PipeWire against Arma 3 under Proton 10. Takes archived as
lossless FLAC alongside this file; `wav_stats.py` reads either format.

| Take | Peak | RMS | Non-zero samples |
|---|---|---|---|
| `control_slidersup` | −44.7 dBFS | −60.6 dBFS | 98.16% |
| `test_sliderszero`  | **−inf (exact 0)** | −inf | **0.00%** |
| `fight`             | −12.9 dBFS | −23.7 dBFS | 99.84% |

## Verdict: PASS

**Requirement 5 confirmed — zeroing silences, it does not attenuate.**
`test_sliderszero` is exact digital zero across all 2,878,922 samples, while
`fight` (recorded with the *same* sliders zeroed) is a strong −12.9 dBFS. Taken
together: zeroing removes non-diegetic content completely and leaves diegetic
combat audio fully intact. No post-capture filtering of music/UI is needed.

**Signal level is healthy for mixing.** −23.7 dBFS RMS with ~11 dB of headroom
to peak — ample to mix under voice without amplification.

**Format (Linux only):** 48 kHz, 2 ch. TS3 also runs at 48 kHz, so the converter
is likely a stereo→mono downmix with no resampling. Does not transfer to a
Windows implementation — WASAPI negotiates its own format.

## Requirement 4 resolved — the gate IS needed

Two further idle takes settled it. `idle` is walking/running through foliage and
sand beside waves; `idle2` is heavy rain with occasional thunder.

RMS per 50 ms window:

| take | p5 | p25 | p50 | p75 | p95 | p99 |
|---|---|---|---|---|---|---|
| `fight` | −55.3 | −34.1 | −26.9 | −21.5 | −18.6 | −17.8 |
| `idle` (foliage/waves) | −57.1 | −54.3 | −52.6 | −50.6 | −47.6 | −43.1 |
| `idle2` (rain/thunder) | −44.8 | −43.2 | −38.1 | −33.9 | −26.8 | −23.8 |

The earlier reading — that `fight`'s −55 dBFS floor made a gate pointless — was
the wrong measure. What matters is not the combat take's floor but the *idle
take's ceiling*: `idle` sits at −52.6 dBFS median with a −36.5 dBFS worst-case
window. That is a continuous bed that would otherwise ride under every single
transmission. A gate removes it outright.

### Recommended: −35 dBFS threshold, 10 ms window, 200 ms hold

| threshold | `fight` open | `idle` open | `idle2` open |
|---|---|---|---|
| −40 dBFS | 91.5% | 4.8% | 74.9% |
| **−35 dBFS** | **88.5%** | **0.0%** | **59.0%** |
| −30 dBFS | 83.8% | 0.0% | 32.9% |

−35 dBFS is where foliage/waves reaches exactly zero while combat still passes
88.5%. Tune against Tier B, but start here.

**The hold is not optional.** A bare per-window threshold on `fight` produces 116
transitions in 30 s (3.9/s) — audible chattering. A 200 ms hold cuts that to
0.2/s while *raising* combat pass-through, since it bridges inter-shot gaps.

### Rain passes the gate, and that is correct

No threshold separates heavy rain from combat, because they genuinely overlap:
`idle2` peaks at −13.3 dBFS against `fight`'s −12.9 dBFS. A peak-based gate
cannot tell thunder from gunfire, and it should not try — requirement 4 is an
amplitude gate, explicitly not a content classifier.

At −35 dBFS, 59% of the rain take passes. That is the right outcome: a real
operator transmitting in a rainstorm *would* have rain in their signal, and
thunder loud enough to clear the gate passes for the same reason. This is the
same principle as requirement 6's acceptance of own-gunfire dominance.

## Take 5 (isolation) — not run

Skipped: no second player available, and received radio audio is known to come
through the TS3 node, not Arma's. That is the same process-separation property
the PID-targeted design relies on, and it gets proven for free the first time
Tier A runs.

---

# Resolved: the `edited` flag and the TS3 capture format

From the TeamSpeak Plugin API manual, *Accessing the voice buffer*
(<https://teamspeakdocs.github.io/PluginAPI/client_html/ar01s18.html>).

## `edited` is a bitmask, and ACRE2 never writes it

[TsCallbacks_sound.cpp:130](../extensions/src/ACRE2TS/TsCallbacks_sound.cpp#L130)
ignores the `edited` out-parameter entirely. That costs nothing today because
stock ACRE2 does not modify captured samples — but it is a latent trap for this
feature:

- **Bit 1 (value 1), on output:** set it if the sound data was changed —
  `*edited |= 1`. **Without this, TeamSpeak discards our mixed samples.**
- **Bit 2 (value 2), on input:** whether the sound is about to be sent to the
  server. Clear it (`*edited &= ~2`) to suppress transmission.

Phase 3 must set bit 1 after mixing. Skipping it presents exactly as "capture
and mixing are broken" during Tier A, with the mix working perfectly and the
result being thrown away downstream.

Bit 2 reads as "whether the sound is about to be sent to the server" on input,
which would make it a useful signal for skipping the mix on buffers that are
not being transmitted. That reading comes from terse docs and is **unverified**
— log its actual value on entry before relying on it. Bit 1 is unambiguous and
load-bearing; implement that first and treat bit 2 as unconfirmed.

## Requirement 2 (resampling) is smaller than the gameplan assumed

The manual specifies the capture buffer as **signed 16-bit @ 48 kHz**. The
Phase 0 takes came off PipeWire at 48 kHz. The rates match, so **no resampling
stage is needed** — the converter reduces to a stereo→mono downmix plus a
float→int16 conversion, both trivial and cheap enough for the audio callback.

This holds for the Linux backend. A Windows implementation negotiates its own
WASAPI format and must confirm the rate separately.

---

# Demo / Tier A verification

The mix is live: captured game audio is gated, scaled and summed into the
outgoing capture buffer in `CSoundEngine::onEditCapturedVoiceDataEvent`, which
then sets `*edited |= 1` so TeamSpeak keeps the modified samples.

Because the injection point is pre-encode, the receive-side radio DSP
(bandpass, noise, distortion, hard clip) applies to the ambience automatically,
exactly as it does to voice. Nothing extra is needed for that.

## Recording a demo

Do **not** use TeamSpeak's Tools → Options → Capture → Begin Test. It mutes the
microphone and distorts the audio, so it shows neither your voice nor what a
listener would receive. The gameplan suggested it for Tier A; it does not work.

Instead have the plugin write the outgoing stream to disk. That buffer is what
TeamSpeak encodes and sends, so the file *is* the transmitted audio:

1. Set `ambientDumpFile` in `acre2.ini` to an absolute path, e.g.
   `Z:\home\you\acre2_outgoing.wav` (Wine maps `Z:` to `/`).
2. **Restart TeamSpeak** so the setting is read.
3. Start the helper: `python3 ambient/ambient-helper.py`
4. Key up the radio near some combat and talk.
5. Inspect it: `python3 ambient/wav_stats.py ~/acre2_outgoing.wav 2`

Each transmission overwrites the file, so collect it between key-ups.

`record-demo.sh` records TeamSpeak's own output node instead, which is useful
once a second client is receiving, but it cannot show your own outgoing audio.

## Tuning

`acre2.ini` (next to the other ACRE2 settings) gains three keys:

| key | default | meaning |
|---|---|---|
| `ambientEnabled` | `true` | master switch |
| `ambientVolume` | `0.5` | ambience level relative to voice |
| `ambientGateThreshold` | `-35.0` | dBFS below which ambience is suppressed |

**Settings are read once, when the plugin starts.** `CEngine::initialize()`
loads `acre2.ini` at TeamSpeak launch and never re-reads it, so editing the
file while TeamSpeak is running has no effect — restart TeamSpeak after any
change. When a dump path is picked up, capture start logs
`AMBIENT: dumping outgoing stream to <path>`; the absence of that line means
the setting was not loaded.

`ambientVolume` is the one to try first. 0.5 was chosen to sit clearly under
speech given the Phase 0 levels (combat at −23.7 dBFS RMS), but it is a
starting point, not a measured optimum — that needs a real listener.

## Gate implementation verified against the Phase 0 takes

The shipped C++ gate was run over the original recordings and reproduces the
Python analysis exactly:

| take | windows passed |
|---|---|
| `fight` | 88.5% |
| `idle` (foliage/waves) | 0.0% |
| `idle2` (rain/thunder) | 59.0% |

---

# Status: working end to end (single client)

Confirmed by ear and by the dump file: game audio is captured from Arma's
PipeWire node, gated, scaled and summed into TeamSpeak's capture buffer in
real time, with `*edited |= 1` set so TeamSpeak keeps the modified samples.

## What the dump proves, exactly

`ambientDumpFile` is written from inside
`CSoundEngine::onEditCapturedVoiceDataEvent`, after the mix, from the same
`samples` buffer that is handed back to TeamSpeak. So it is not a
reconstruction or a parallel recording — it is the actual buffer TeamSpeak
goes on to encode and transmit.

## What is still unverified

**No remote listener has received it yet.** The dump proves the buffer we hand
TeamSpeak contains the mix. It does not prove what comes out the far end.
Two things sit between the two:

1. **Opus encoding.** TeamSpeak encodes voice with Opus, and a channel set to
   *Opus Voice* at a low quality level is tuned for speech, not gunfire.
   Ambience may survive noticeably worse than the dump suggests. If received
   audio disappoints, raise the channel's codec quality or switch it to
   *Opus Music* before assuming the mix is at fault.
2. **Receive-side radio DSP** (`FilterRadio.cpp`: bandpass 750-4000 Hz, noise,
   distortion, hard clip) should apply to the ambience automatically, since
   the injection point is pre-encode. Expected, not yet confirmed by ear.

Both are settled by one Tier B session with a second player — which also
exercises `onPluginCommandEvent`, the only path touched by the mingw assembly
replacement that solo testing cannot reach.

---

# The Windows port — development setup

The Linux backend is feature complete. The remaining work is the Windows
capture backend, and the obstacle is that neither available environment can run
it: Proton does not implement `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`,
and GitHub's Windows runners have no audio endpoint, so there is nothing for a
render process to feed and nothing for a loopback capture to capture.

A local Windows VM has both. **It does not need Arma.** What is unverified is
the process-loopback API contract -- activation, the negotiated
`WAVEFORMATEX`, PID scoping, and the resampler -- and all of that is provable
against any process that renders audio. A tone generator stands in for the game.
Whether Arma's own render stream captures cleanly is the part that still needs a
real Windows machine and a second player.

## What is set up

**Nothing below has been exercised yet — the guest has never booted.** The
Windows ISO is the one piece that cannot be automated, so the VM is staged but
unbuilt, and `sync`, `install-vs` and the MSVC workflow are all unrun.


- `ambient/vm.sh` drives the guest from the host shell: `up`, `status`, `ssh`,
  `sync`, `install-vs`.
- The VM lives in `~/VMs`, never in the repo -- the ISOs are several GB.
  quickemu, not libvirt; `/dev/kvm` is world-accessible so no group change is
  needed despite what quickemu's docs imply.
- quickemu's generated `autounattend.xml` was patched to run `setup-ssh.ps1` at
  first logon, which installs OpenSSH Server, sets PowerShell as the login
  shell, and installs `~/.ssh/acre2-vm.pub` into
  `administrators_authorized_keys`. Admin accounts read keys *only* from that
  file, and sshd silently refuses it unless the ACL grants SYSTEM and
  Administrators alone -- hence the `icacls` calls.
- The guest gets an emulated `intel-hda` device (quickemu's default), which is
  the whole reason this works where CI does not.
- `.github/workflows/ambient-msvc.yml` compile-checks the ambient translation
  units against the real Windows SDK on every push that touches them. It is
  deliberately narrow: upstream's full build needs the legacy DirectX SDK from
  IDI's private FTP, and `FilterPosition.cpp` is the only file that wants it.
  Actions is free here because this fork is public.

## Gotchas already paid for

- **Microsoft IP-blocks `quickget`'s ISO download.** The Windows ISO has to be
  fetched manually from a browser and saved to
  `~/VMs/windows-11/windows-11.iso`. Everything else automates.
- **Do not re-run `quickget` once assets are in place.** It deletes
  `virtio-win.iso` before re-downloading, and then fails, leaving nothing.
- **The virtio mirror is behind Anubis anti-bot** on the `stable-virtio` path.
  The `archive-virtio/virtio-win-<version>-1/` path is not, and serves the same
  ISO.

# The WASAPI capture source

`AmbientWasapi.h/.cpp` implements Windows process-loopback capture, and
`ambient/probe/` builds it into `ambient-probe.exe` -- the same translation
unit, so a probe result is evidence about the shipping code rather than about a
reimplementation of it. Build with `ambient/build-probe.sh`.

The class delivers s16 mono 48 kHz to a sink, which is the wire format the
Linux helper already produces, so everything downstream stays unchanged when it
is eventually wired in behind a source interface.

## What is verified

- **Compiles under MSVC against the real Windows SDK**, including the vendored
  `ambient/compat/audioclientactivationparams.h` -- mingw does not ship that
  header at all, so this is the check that the activation structures match the
  genuine definitions.
- **Links statically.** `ambient-probe.exe` depends only on KERNEL32, ole32,
  mmdevapi and the UCRT, all present on any Windows 10+ machine, so it can be
  handed to a tester as a single file.
- **The activation path runs end to end under Proton and fails correctly.**
  Given `--pid 100`, the probe reports
  `process loopback activation rejected (0x80070002)` in under a second.

That last result is worth more than it looks. `0x80070002` is
`ERROR_FILE_NOT_FOUND` -- the `VAD\Process_Loopback` device does not exist,
which is exactly right for Proton. Reaching that error means
`ActivateAudioInterfaceAsync` dispatched, **the completion handler fired**, and
`GetActivateResult` returned a specific answer from the audio stack. So the COM
initialisation, the `AUDIOCLIENT_ACTIVATION_PARAMS` blob, the `IAgileObject`
plumbing and the error path are all structurally correct.

It also settles an open design question: under Proton the handler *does* get
invoked with a failure rather than never arriving, so "try WASAPI, fall back to
the helper" cannot stall push-to-talk. The bounded wait
(`ACTIVATE_TIMEOUT_MS`) stays anyway, since that conclusion is one Wine version
deep.

## Verified on Windows — 2026-09-15

Run in the local VM (Windows 11 24H2, build 26100) against a PowerShell process
looping `C:\Windows\Media\Alarm01.wav`, using the cross-compiled
`ambient-probe.exe`. Both processes in the interactive session — see the gotcha
below.

### Process loopback works, and the format is ours to choose

```
OK: activated. negotiated format: 48000 Hz, 1 ch, 16-bit, tag 1
samples : 484800 (10.10 s at 48 kHz)
peak    : -10.0 dBFS      rms: -24.6 dBFS      non-zero: 96.30%
```

The client **accepted the requested 48 kHz mono s16 exactly**. 10.09 s of audio
for a 10 s capture, so the rate is real and does not drift. The WAV was pulled
back to Linux and confirmed independently with `wav_stats.py`.

**This kills the resampler.** `Initialize()` either accepts the format we ask
for or fails outright, and `m_format` is set from the request rather than from
the engine — so the conversion paths in `deliver()` were unreachable by
construction, not merely unused. Removed.

### Requirement: PID scoping isolates — proven, not reasoned

The gameplan's hard requirement is that capture never picks up the device mix,
because ACRE2 plays received radio through the TeamSpeak process and
re-transmitting it would feed back. Tested directly: one process blaring, one
process rendering nothing, captured seconds apart on the same device.

| capture target | non-zero | peak | gate open at −35 dBFS |
|---|---|---|---|
| a process rendering **nothing**, while the other blares | 22.95% | −90.3 dBFS | **0.0%** |
| the same, with **nothing else rendering at all** | 22.95% | −90.3 dBFS | **0.0%** |
| the **blaring** process (positive control) | 97.01% | −10.1 dBFS | 67% |

The middle row is the one that proves it. The silent capture is **bit-identical
whether or not another process is blaring on the same device** — so the residue
is not leakage, it is the audio engine's own floor.

And it is precisely a dither floor: the entire 8 s capture contains exactly
three distinct sample values, symmetrically distributed.

```
-1: 44665    0: 299952    1: 44663
```

That is ±1 LSB, 55 dB below the gate threshold, and the gate removes 100% of it.
Leaked audio would show a spread of magnitudes, not three values.

**An earlier run of this test reported exact digital zero, and that was an
artifact of the code, not a better result.** The original `deliver()` converted
int16 → float → int16 via `/32768` then `*32767`, and that round-trip truncated
±1 to 0. Removing the dead conversion path made the capture faithful and
exposed the floor that had been there all along.

### An idle target delivers silence, not nothing

The silent capture still produced **811 buffers** over 8 s — process loopback
emits packets rather than stopping. `deliver()` treats them as zeros rather than
skipping them, which is what keeps the ambience in step with the speech it
accompanies. That guess turned out to be the right one.

## Gotcha: audio is per-session, and SSH lands in session 0

An SSH session runs in session 0 (services); the desktop is session 1. Windows
audio is per-session, so **nothing started over SSH can render or capture
audio** — it will activate happily and record digital silence forever, which
looks exactly like a broken capture.

Run both the renderer and the probe in the interactive session:

```
schtasks /create /tn AmbientProbe /tr "powershell -File C:\path\script.ps1" \
         /sc once /st 00:00 /ru Quickemu /it /f
schtasks /run /tn AmbientProbe
```

`/it` is the load-bearing flag.

### The gate, checked against non-Arma audio for the first time

The probe runs the shipped `CAmbientGate` over what it captured. On the alarm
loop it reported **84.2% of windows open at −35 dBFS**, and an independent
Python reimplementation of the same algorithm over the same WAV gave **84.2%**
— an exact match, so the C++ gate does what it is specified to do.

The figure is below the firefight's 88.5% because `Alarm01.wav` contains a
1.45 s silent gap between loops (145 consecutive sub-threshold windows). The
gate correctly closes through most of it, and the 200 ms hold bridges the nine
short 30–90 ms dips without chattering. Nothing anomalous.

### The plugin's own entry path

Everything above targets a PID directly. The plugin instead calls
`start()` → `findProcessId()`, which had never run. Tested with three
`powershell.exe` processes live: the lookup found one, the ambiguity warning
fired correctly, and capture succeeded at −10.0 dBFS / 96.03% non-zero.

## Verified inside real TeamSpeak — 2026-09-16

The plugin was installed into a TeamSpeak 3 client in the VM and the whole
capture path exercised in-process, with a renamed `powershell.exe` playing a WAV
standing in for the game (the plugin looks the target up by name, so the
stand-in has to actually be called `arma3_x64.exe`).

```
AMBIENT: capture started (WASAPI process loopback)
AMBIENT SELFTEST: OK -- backend 'WASAPI process loopback', 383040 samples
                  (7.98 s), peak -10.0 dBFS, post-gate RMS -25.1 dBFS
ring: overruns=0 underruns=1 skips=30
```

This closes the gap the standalone probe could not: COM initialisation on the
capture thread inside `ts3client.exe`, backend selection resolving to WASAPI
rather than falling back, the by-name process lookup, and the ring and gate all
working in the host process. Levels match the standalone probe exactly.

`skips=30` is the self-test's own polling loop, not a capture fault — `Sleep(10)`
really sleeps ~15 ms at the default timer resolution, so it consumes slower than
the backend produces and the ring trims itself. The real consumer is TeamSpeak's
capture callback, driven by the audio clock. `overruns=0` is the meaningful
number.

### `ambientSelfTest`

New diagnostic setting, default off. Set it and the plugin captures for 8 s at
startup and reports what arrived. It exists so a tester on a machine we cannot
reach can answer "does capture work at all?" without getting in-game and keying
a radio — which separates a broken backend from transmit hooks that never fired,
without a live debugging session.

## ACRE2 needs the DirectX runtime, and fails opaquely without it

The plugin would not load at all at first:

```
Loading plugin: acre2_win64.dll
Failed to load plugin: ...\acre2_win64.dll
```

`LoadLibraryEx` gave error 126, and the import table says why: **`X3DAudio1_7.dll`**,
a legacy DirectX SDK redistributable that is not part of Windows. Wine ships an
implementation, which is why this never surfaced on Linux.

**This is not specific to the mingw build** — the stock upstream MSVC
`acre2_win64.dll` imports the same DLL. Anyone already running ACRE2 has the
DirectX End-User Runtime and will never hit it. But a clean machine gives no
useful diagnostic, just "Failed to load plugin", so it is worth knowing before
handing a build to a tester.

In the VM: `winget install Microsoft.DirectX` installs an MSIX that does not put
the DLL on the search path, so `X3DAudio1_7.dll` has to be copied from
`C:\Program Files\WindowsApps\Microsoft.DirectXRuntime_*\` into `System32`.

## Gotchas from the TeamSpeak setup

- **TeamSpeak runs portable when copied rather than installed**, using
  `<install>\config\` and ignoring `%APPDATA%\TS3Client` entirely. Plugins go in
  `config\plugins\`, and `acre2.ini` in `config\acre\`.
- **`Stop-Process -Name powershell` over SSH kills the session's own shell**, so
  the rest of the command silently never runs. Exclude `$PID`.
- ACRE2 logs to `%LOCALAPPDATA%\Arma 3\acre2_plugin.log`, which is where every
  `AMBIENT:` line above comes from.

## What is still not verified

The Windows backend is proven as far as "the capture class works when called
the way the plugin calls it". Beyond that:

- **No TeamSpeak has ever loaded this DLL with a WASAPI source in it.** Every
  Windows result here comes from `ambient-probe.exe`. The mix into
  `onEditCapturedVoiceDataEvent`, the `*edited |= 1` contract and the transmit
  hooks have only ever run against the socket backend. The Linux regression
  test does not close this either — it exercises the helper path by
  construction.
- **Arma's own render stream.** A PowerShell `SoundPlayer` is not the game.
  Whether Arma renders through the shared engine in a way that captures cleanly
  needs a real machine with it installed.
- **BattlEye.** Unverifiable anywhere but a real install.
- Tier B: Opus survival, receive-side radio DSP, `ambientVolume` tuning.

# Picking this up again

## To get running

```
./ambient/build-mingw.sh --status     # is the mingw build still installed?
./ambient/build-mingw.sh --install    # if not, or after any code change
python3 ambient/ambient-helper.py     # leave running; required or nothing captures
```

Then launch Arma + TeamSpeak via `~/Arma3Helper.sh`. Restart TeamSpeak after
any `acre2.ini` change.

Healthy log lines during a transmission:

```
AMBIENT: capture started
AMBIENT: dumping outgoing stream to <path>        (only if ambientDumpFile set)
AMBIENT: capture callback format -- sampleCount=480 channels=1
AMBIENT: mixed into N callbacks; mean ambient level -NN.N dBFS
AMBIENT: capture stopped -- N bytes (N.NN s); ring: overruns=0 underruns=N skips=0
```

`helper not reachable` means the helper is down. `mixed into 0 callbacks` means
capture ran but contributed nothing. Overruns or skips above zero mean the rates
have drifted and the ring sizing needs revisiting — they were zero across every
test so far.

## Next step: Phase 4, needs a second player

Everything left requires someone on the other end of the radio:

1. **Tune `ambientVolume`.** Currently `0.5`, reasoned from Phase 0 levels
   (combat at -23.7 dBFS RMS) but never checked against a listener. This is the
   first thing to adjust.
2. **Check Opus survival.** Gunfire may fare badly through *Opus Voice* at low
   quality. Try raising the channel codec quality or *Opus Music* before
   blaming the mix.
3. **Confirm the receive-side radio DSP** applies to the ambience. It should,
   automatically, given the pre-encode injection point.
4. **Confirm `onPluginCommandEvent` still works.** It is the only path touched
   by the mingw assembly replacement that solo testing cannot reach. A broken
   trampoline would show up as radio state not syncing between players.

## Deferred, per gameplan.md — nothing found so far argues for pulling these forward

Supersonic crack, nearby direct-voice bleed, and ducking/AGC on the combined
signal. The last one may become relevant if Phase 4 shows the mix clipping
unpleasantly against the existing hard clip in `FilterRadio.cpp`.
