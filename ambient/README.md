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
| `ambientVolume` | `0.26` | ambience level relative to voice |
| `ambientGateThreshold` | `-35.0` | dBFS below which ambience is suppressed |
| `ambientSelfTest` | `false` | capture for 8 s at startup and report to the log |
| `ambientDumpSignalQuality` | `0.0` | above zero, run the dump through the receive-side radio DSP at this signal quality (0–1) |

## Making the dump sound like what a listener hears

The dump is the *transmitted* buffer, captured before Opus and before the
receiving client applies its radio DSP — so it sounds clean and dry, not like
radio. Set `ambientDumpSignalQuality` above zero and the dump is additionally
run through `CRadioEffect`, **the same object the receive path uses**, not a
reimplementation of its maths:

```
ambientDumpSignalQuality = 0.9;
```

0.9 is a strong, clean signal. Lower values add more pink and white noise,
more ring modulation and more foldback distortion, because the filter scales
all three by `1.25 - quality`. Zero disables the whole stage and also means
silence in-game, which is why zero is the "off" value here.

**It is a close simulation, not the real thing.** The Opus round trip sits
between transmit and receive in reality and cannot be reproduced from the
transmitting side, so a demo made this way will sound slightly better than what
a listener actually gets.

**Settings are read once, when the plugin starts.** `CEngine::initialize()`
loads `acre2.ini` at TeamSpeak launch and never re-reads it, so editing the
file while TeamSpeak is running has no effect — restart TeamSpeak after any
change. When a dump path is picked up, capture start logs
`AMBIENT: dumping outgoing stream to <path>`; the absence of that line means
the setting was not loaded.

`ambientVolume` is the one to try first. It started at 0.5, reasoned from the
Phase 0 levels (combat at −23.7 dBFS RMS), and is now 0.26 — still a starting
point rather than a measured optimum, which needs a real listener.

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

# Ambient-bus DSP — mic model and compressor

**Settled configuration, 2026-09-16.** These are the defaults, so a bare
`python3 ambient/ambient-helper.py` is the tuned chain:

| stage | value | what it does |
|---|---|---|
| high-pass | 300 Hz, 12 dB/oct | noise-cancelling mic model; buys headroom |
| compressor | −28 dBFS, 8:1 | squashes anything above the quiet bed |
| attack / release | 1 ms / 40 ms | fast enough to catch gunfire, short enough to recover between shots |
| makeup | **+4 dB** | flat lift after compression |
| `ambientVolume` | **0.26** (acre2.ini) | independent overall level |

`--no-dsp` bypasses the whole chain for A/B. Every parameter is a float, so
tuning happens in 1 dB steps or smaller.

**`--makeup` is the day-to-day level knob, not `ambientVolume`.** Because the
compressor is holding the peaks, raising makeup lifts the quiet bed without the
gunfire coming back with it. Lowering `ambientVolume` instead pulls everything
down equally and takes the bed out first — which is the trap this whole stage
exists to avoid.

## How it got to +4 dB

Worth recording, because the first attempt went wrong in an instructive way.

With the chain in and no makeup, the mix measured well — spread roughly halved
on every take, and the `inbound` demo stopped clipping at 0.0 dBFS — but **the
rotor wash in a Huey became inaudible**. The instinct was that the high-pass had
eaten it, and that was wrong: measured band by band, the change was nearly
uniform (−6.6 dB below 300 Hz, −8.0 at 750–1200, −6.9 at 3–4 kHz). No spectral
tilt at all.

The real cause was simpler. `FilterRadio` high-passes at 750 Hz on the receive
side, so sub-750 rotor content was never reaching the listener in the first
place — the 300 Hz filter cannot remove what the radio already removed. What
actually happened is that the ambience lost about 7 dB against a voice that did
not change, and the rotor bed had been sitting just above audibility. Compression
plus a flat 25% cut were pulling in the same direction, and the quietest thing in
the mix went first.

Makeup gain is the fix for exactly that: squash the peaks, then lift everything.
+6 dB put the bed 3.7 dB *above* where it started while leaving the gunfire
5.8 dB below. +4 dB is where it landed after that turned out slightly hot.

`ambient/ambient_dsp.py` implements two stages that belong on the **ambience
only**, never the summed signal. They live in the helper rather than the plugin
for exactly that reason — the helper carries Arma's audio and nothing else — and
because tuning there costs a Python restart instead of a DLL rebuild plus a
TeamSpeak restart.

- `python3 ambient/ambient-helper.py --dsp` — live, in-game
- `./ambient/dsp-preview.py <file> --compare -o out.wav` — offline A/B

**Preview only on pure ambience** (`ambient/fight.flac`, `idle.flac`,
`idle2.flac`). An `ambientDumpFile` recording is voice summed with ambience and
has already been through the radio filter, so the mic model appears to do
nothing and the compressor ducks the voice, which it never does in production.

## The mic model earns its place on measurement, not realism alone

Real tactical headsets use noise-cancelling pressure-gradient microphones, which
reject the far field most strongly at low frequencies. So ambience reaching a
real radio is high-passed.

That happens to matter far more than it sounds, because of where the energy is:

| take | <300 Hz | 300–750 | 750–4k | >4k |
|---|---|---|---|---|
| `fight` (pure ambience) | **76.3%** | 7.8% | 11.9% | 4.0% |
| `idle2` rain (pure ambience) | **92.9%** | 3.1% | 3.2% | 0.8% |
| a dump, post radio filter | 0.8% | 21.8% | 65.9% | 11.5% |

ACRE2's receive filter high-passes at 750 Hz, so that low end is **discarded
before anyone hears it** — after it has eaten headroom at the mix, driven
`FilterRadio`'s 3× boost into foldback, and clipped. One of the demos peaks at
0.0 dBFS. Removing it up front is close to inaudible in the result and buys back
a lot of room: −11.3 dB RMS on `fight`, −8.8 on rain, but only **−1.7 on
foliage and waves**, which is HF-rich and survives almost untouched. The quiet
bed is the thing that stays.

## The compressor, and where realism loses

Threshold must sit **above the quiet bed and below the loud events**. Below the
bed it is only an attenuator with extra steps — that mistake shows up as the
whole signal dropping by the gain reduction, bed included.

Post-high-pass on `fight`, the bed is −34.3 dBFS and the loud events −21.6, so
−28 dB threshold. Holding that with 8:1 and a 1 ms attack, and sweeping release:

| release | bed | loud events | spread |
|---|---|---|---|
| none | — | — | 12.8 dB |
| **40 ms** | **−2.0 dB** | **−6.9 dB** | **7.8 dB** |
| 150 ms | −4.5 | −8.2 | 9.1 |
| 300 ms | −5.8 | −9.0 | 9.6 |
| 600 ms | −7.1 | −9.6 | 10.3 |

Monotonic, and it settles the VOGAD question. A real Voice-Operated Gain
Adjusting Device has a slow release, and that is measurably **worse** here:
sustained fire never gives it time to recover, so it holds the gain down through
the quiet parts and ducks the bed by nearly as much as the gunfire. 40 ms keeps
the bed while still taking 7 dB off the loud events.

Realism is a good source of ideas here and a bad source of final values.

## Recording ambience and voice separately, for offline A/B

**Development builds only.** This records the player's raw microphone to disk,
so it is a compile-time opt-in rather than a setting:

```
./ambient/build-mingw.sh --dev --install     # enable it
./ambient/build-mingw.sh --install           # back to a safe build
```

Without `--dev` the setting, the code and the string are all absent from the
binary — a comment saying "diagnostic only" is not strong enough for something
that writes somebody's microphone to a file, and this is the one feature here
that must never reach a machine that is not this one.

Every build prints which it is, read from the binary rather than the flags, so a
stale build directory cannot quietly disagree:

```
ok: no development diagnostics in this build -- safe to distribute
WARNING: this build contains the development split dump (microphone recording).
```

With a dev build installed, `ambientDumpSplitFile` writes a **stereo** file:
ambience left, microphone right, both captured at the mix site before they are
summed.

```
ambientDumpSplitFile = Z:\home\liam\acre2_split.wav
```

The ambience channel is taken **before the noise gate**, so gate settings can be
varied offline too. Record with the helper on `--no-dsp`, or its chain is baked
in before the plugin ever sees the audio and you are A/Bing on top of a
processed signal.

That gives real gameplay as two aligned tracks, which is a far better test bed
than the Phase 0 takes for anything about how ambience sits against speech —
those recordings contain no voice at all.

## Before handing a build to anyone else

1. `./ambient/build-mingw.sh --install` — **without** `--dev`. Confirm the
   output says *no development diagnostics in this build*.
2. Check the ini you ship or describe has no `ambientDumpSplitFile`, and that
   `ambientDumpFile` is empty unless the tester is meant to be recording.
3. `ambientSelfTest` is genuinely meant to ship — it is how a tester answers
   "does capture work at all" without getting in-game. Leave it.
4. Rebuild `~/acre2-ambient-test.zip` from that clean build, since the existing
   one is only as current as the last time it was made.

## Reality check: how close is any of this to a real radio?

Honest answer: **it is sound design that borrows real mechanisms, not
simulation.** Worth writing down so nobody later mistakes the vocabulary for
accuracy.

**Where it diverges, stage by stage:**

- **The high-pass has the right mechanism and the wrong numbers.** A real
  pressure-gradient mic rolls far-field sound off at about **6 dB/octave**, and
  its rejection is **finite** -- typically 10-20 dB. Ours is a 12 dB/oct
  Butterworth heading to −∞. Twice as steep as physics, with no floor.
- **The compressor is not a VOGAD.** A real one sits on the *combined* mic
  signal, so a gunshot ducks the operator's voice; ours is ambience-only and
  never does. Real release is 0.5-3 s against our 40 ms, and a VOGAD targets
  constant average modulation rather than following a threshold/ratio curve.
  Functionally ours is a modern broadcast limiter wearing a military name.
- **Makeup and `ambientVolume` have no physical analogue at all.**
- **The gate is actively unphysical.** It silences quiet ambience while the
  voice continues, which one microphone cannot do.

**The architectural unreality underneath all of it:** a real radio has one mic
producing one signal, and the voice-to-ambience ratio is set by physics --
distance, polar pattern, actual SPL. We have two independent signals and choose
the ratio. Every level decision here synthesises something physics would
otherwise dictate.

**What is not modelled at all:** microphone capsule and preamp overload, which
is probably the most characteristic sound of transmitting beside a weapon; the
2-4 kHz presence peak comms mics have for intelligibility; helmet and headset
attenuation ahead of the mic; mechanical coupling, wind and handling noise;
vocoder mangling (real tactical digital radios use MELPe/AMBE, which force audio
through a vocal-tract model and garble non-speech far worse than Opus does);
FM pre/de-emphasis, squelch tails and PTT clicks.

**And the thing underneath is stylised too**, which caps how much realism is
even meaningful. ACRE2's `FilterRadio` passes 750-4000 Hz where real narrowband
voice is roughly 300-3400, so it is thinner than reality. Its ring modulation is
not a radio artifact -- analog degradation is noise, fading and multipath, and
digital is vocoder warble and packet loss. Its foldback is a waveshaper choice,
not overmodulation behaviour.

### If realism is ever worth chasing further

Ranked by payoff against effort:

1. **A low-shelf instead of the high-pass** -- roughly 15 dB of cut below
   300 Hz rather than a true filter. More accurate *and* it gives back some
   rotor character, which the current filter removes entirely. Cheapest real
   improvement available.
2. **Soft saturation before the compressor**, modelling mic overload on
   transients, so close gunfire sounds mic-slammed rather than merely loud.
3. **Sidechain duck from the voice.** Not literally what a single mic does, but
   it produces the outcome a single mic produces -- the near source dominating
   the far one.

### The caveat that matters most

**Realism has already been wrong here once, measurably.** The authentic slow
VOGAD release made the thing being optimised for worse, and the measurement
said so plainly. Realism has been an excellent source of *ideas* -- the mic
model came from it and was the single most valuable change -- and an unreliable
source of *values*.

The working rule: chase realism where it happens to produce fun, and drop it the
moment the numbers disagree.

## The DSP is Linux-only, and that matters for the friend test

The chain lives in `ambient-helper.py`, which **only the Linux backend uses**.
`CAmbientWasapiSource` captures in-process and never touches the helper, so a
Windows transmitter currently gets **raw, untuned ambience** — no mic model, no
compressor, no makeup, and `ambientVolume` alone doing the work.

Consequences, in order of how likely they are to bite:

- **Test A is unaffected.** You transmit from Linux, so everything above
  applies. Tuning results from that session are real.
- **Test B would not sound like this.** A friend running the package as the
  transmitter would hear the pre-DSP behaviour: gunfire 6 dB hotter, clipping at
  0.0 dBFS on busy scenes, and the wide dynamic range that motivated all of
  this. Worth saying to them up front, or the feedback will be about a version
  that no longer exists here.
- **Porting it is the obvious next code task.** `AmbientGate` and
  `AmbientRingBuffer` are already shared C++; the chain is a biquad and a
  compressor, neither of which is hard to write twice. The parameters are
  settled and measured, so it is a transcription job rather than a design one.
  It would also let the values move into `acre2.ini` instead of living as
  helper command-line defaults.

# Picking this up again

*Current as of 2026-09-16.*

## Where this stands in one paragraph

The feature works. Game audio is captured, gated, scaled and mixed into the
outgoing TeamSpeak buffer pre-encode, on **both** backends: the Linux helper
socket (used daily here, since Proton has no process loopback) and WASAPI
process loopback (verified in a Windows VM, including inside a real TeamSpeak
process). Everything downstream of the capture backend is shared between the
two and covered by sanitizer tests in CI. What has never happened is a **second
person on the other end of the radio**, and that is the only thing standing
between here and done.

## To get running

```
./ambient/build-mingw.sh --status     # is the mingw build still installed?
./ambient/build-mingw.sh --install    # after any code change; writes BOTH locations
python3 ambient/ambient-helper.py     # leave running, or nothing captures
./ambient/run-tests.sh                # ring buffer + gate, under ASan/UBSan/TSan
```

The helper applies the tuned DSP chain by default now — no flags needed, and
the line it logs at startup states the whole chain, so if it does not mention
the compressor and makeup you are not running what you think you are.

Then launch Arma + TeamSpeak via `~/Arma3Helper.sh`. **Restart TeamSpeak after
any `acre2.ini` change** — it is read once at plugin start.

`acre2.ini` lives at
`…/compatdata/107410/pfx/drive_c/users/steamuser/AppData/Roaming/TS3Client/acre/acre2.ini`.
The working demo configuration is:

```
ambientVolume = 0.26;
ambientGateThreshold = -35;
ambientDumpFile = Z:\home\liam\acre2_demo.wav
ambientDumpSignalQuality = 0.9;
```

Reach for `--makeup` on the helper before `ambientVolume` when the level is
wrong -- the compressor is holding the peaks, so makeup lifts the quiet bed
without the gunfire returning. See the DSP section.

Healthy log lines (`%LOCALAPPDATA%\Arma 3\acre2_plugin.log`, i.e. inside the
prefix) during a transmission:

```
AMBIENT: capture started (helper socket)
AMBIENT: dumping outgoing stream to <path>
AMBIENT: dump will carry the receive-side radio effect at signal quality 0.90
AMBIENT: mixed into N callbacks; mean ambient level -NN.N dBFS
AMBIENT: capture stopped -- N bytes; ring: overruns=0 underruns=N skips=0
```

**The first key-up of a TeamSpeak session has no ambience, by design.** The
backend probe resolves on that transmission and the fallback takes effect from
the second onward. Do not chase it as a bug. `overruns` above zero is the
number that would actually matter; it has been zero in every test.

## What is proven, and by what

| claim | evidence |
|---|---|
| capture → gate → mix → transmit works on Linux | daily use; `acre2_demo.wav` records correctly post-refactor |
| the same works on Windows via process loopback | VM, standalone probe and in-process self-test |
| PID scoping cannot pick up the device mix | silent-process capture is bit-identical with and without another process blaring |
| the engine hands us 48 kHz mono s16 as requested | probe reported the negotiated format; no resampler exists |
| ring buffer and gate are race-free | ThreadSanitizer in CI, 3307 checks |
| the plugin loads and initialises in real Windows TeamSpeak | VM, with DirectX runtime present |

## What is left

Everything remaining needs a second person. **There are two separate tests and
they have very different costs.**

### Test A — tuning. Needs nothing installed on their side.

The mix is entirely transmitter-side (`onEditCapturedVoiceDataEvent`,
pre-encode). The receive path is untouched stock ACRE2. So **you transmit from
Linux and they listen on stock Workshop ACRE2** — no build to send, no GPLv3
distribution, no BattlEye question, nothing for them to install or undo.

This answers three of the four open questions:

1. **Tune `ambientVolume`.** 0.35 now, reasoned rather than measured.
2. **Opus survival.** Gunfire may fare badly through *Opus Voice* at low
   quality. Raise the channel codec quality or try *Opus Music* before blaming
   the mix.
3. **Receive-side radio DSP applying to ambience.** Expected automatically
   given the pre-encode injection point. `ambientDumpSignalQuality` simulates
   it locally but cannot prove it.

Also worth confirming in the same session: **`onPluginCommandEvent`**, the only
path touched by the mingw assembly replacement that solo testing cannot reach.
A broken trampoline shows up as radio state not syncing between players.

### Test B — the Windows backend against real Arma. Needs a package.

Optional, and not required for tuning. Only if you want the Windows capture
path validated against the actual game. A ready-to-send zip is at
`~/acre2-ambient-test.zip` (stripped 1.4 MB DLL, `ambient-probe.exe`, and
instructions written for the tester). Rebuild it if the plugin changes.

Two unknowns that no amount of local work can close:

- **Does Arma render through the shared audio engine?** Process loopback
  captured a PowerShell `SoundPlayer` perfectly, but a game engine may use a
  different render path.
- **BattlEye.** The plugin lives in `ts3client.exe` and never touches Arma's
  memory, so the risk is low by construction, but it is unverified.

Both fail cheaply: `ambient-probe.exe` answers the first in 30 seconds before
anything is installed, and `ambientSelfTest = true` answers it again in one log
line afterwards. Neither needs the tester in-game or you on a call.

## The Windows VM

Lives in `~/VMs` (outside the repo — the ISOs are several GB). Driven by
`ambient/vm.sh`: `up`, `status`, `ssh`, `sync`, `install-vs`. Key is
`~/.ssh/acre2-vm`, guest user `Quickemu`, SSH forwarded to `localhost:2222`.
Windows 11 24H2 with VS Build Tools, the DirectX runtime, and a TeamSpeak 3
client at `C:\TS3\TeamSpeak 3 Client` already installed.

It is **not** needed for day-to-day work — only for touching the WASAPI
backend. If you do:

- Run anything audio-related through `schtasks /it`, never plain SSH. See the
  per-session gotcha above; this is the single easiest way to waste an hour.
- TeamSpeak there runs **portable**: config in `C:\TS3\TeamSpeak 3 Client\config\`,
  plugins in `config\plugins\`, ini in `config\acre\`.
- A renamed `powershell.exe` playing a WAV stands in for the game, because the
  plugin looks its target up by name.

## Architecture, briefly

- `IAmbientSource` — the seam, and the only platform-specific part.
  `CAmbientWasapiSource` (Windows, in-process) and `CAmbientSocketSource`
  (Linux, reads `ambient-helper.py`). Selection is automatic: WASAPI is tried,
  and on failure the helper takes over permanently for that session. Selection
  **never blocks**, because `start()` runs on the push-to-talk path.
- `CAmbientCapture` — everything shared: ring buffer, gate, dump, telemetry.
- `AmbientWasapi.{h,cpp}` deliberately has **no ACRE2 dependencies**, so
  `ambient/probe` can build it standalone. Keep it that way; that property is
  what makes the capture path testable without TeamSpeak or Arma.
- The mix site is `CSoundEngine::onEditCapturedVoiceDataEvent`, and it must set
  `*edited |= 1` or TeamSpeak silently discards the modified samples.

## Deferred, per gameplan.md — nothing found so far argues for pulling these forward

Supersonic crack, nearby direct-voice bleed, and ducking/AGC on the combined
signal. The last may become relevant if Test A shows the mix clipping
unpleasantly against the existing hard clip in `FilterRadio.cpp`.
