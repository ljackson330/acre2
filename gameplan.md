# ACRE2 Ambient Battle Sound — Design

Fork of https://github.com/IDI-Systems/acre2 (GPLv3, Arma 3). Goal: get it
working, not upstream-ready. No need for clean rebase-ability against upstream
`master`.

This document is the **design and the constraints that govern it**. For current
state, evidence, gotchas and how to pick the work back up, see
[ambient/README.md](ambient/README.md) — that is the living document, and it is
authoritative wherever the two disagree.

> This file was originally written as a pre-implementation spec for a
> Windows-only design. It was rewritten on 2026-09-16 to describe what was
> actually built, which has two capture backends rather than one. The original
> is in git history if the reasoning behind a decision is ever needed.

## Feature

When a player transmits on an ACRE2 radio, capture their own Arma 3 process
audio live and mix it into the outgoing transmission, so listeners hear the
transmitter's real combat environment — gunfire, explosions, engines — riding
through the normal TS3 voice pipeline.

## Hard requirement: capture the game process, never the output device

Capture must be scoped to the local Arma process. **Never full-device or
system-wide loopback.** ACRE2's received radio audio plays back through the
TeamSpeak client process, so a device-wide capture would pick up audio this
client is already receiving and re-transmit it, producing a feedback path.

Both backends satisfy this by construction rather than by filtering, and it has
been verified empirically — capturing a process that renders nothing, while
another process blares on the same device, yields the audio engine's dither
floor and nothing else.

## Mechanism — two backends behind one interface

`IAmbientSource` is the seam, and the capture API is the only genuinely
platform-specific part of the feature.

- **`CAmbientWasapiSource`** — Windows. `ActivateAudioInterfaceAsync` with
  `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (Windows 10 2004+, build
  19041+), targeting Arma by PID. Runs in-process inside the TeamSpeak client.
- **`CAmbientSocketSource`** — Linux under Wine. Proton does not implement
  process loopback, so the plugin cannot capture for itself there. A native
  helper (`ambient/ambient-helper.py`) captures Arma's PipeWire node and serves
  it over a loopback socket, which the plugin reads with ordinary Winsock.

Both deliver signed 16-bit mono at 48 kHz — the format TeamSpeak's capture
callback expects — so **everything downstream of the interface is shared**: ring
buffer, noise gate, mixing, dump, telemetry.

Selection is automatic and **never blocks**. `start()` runs on the player's
push-to-talk path, so a backend is chosen by trying WASAPI and letting the
failure arrive on its own thread; the helper takes over permanently for that
session. The visible cost is that the first transmission of a session carries no
ambience, which is a far better trade than a stalled PTT.

## Injection point

- `extensions/src/ACRE2TS/TsCallbacks_sound.cpp` →
  `ts3plugin_onEditCapturedVoiceDataEvent`
- `extensions/src/ACRE2Core/SoundEngine.cpp` →
  `CSoundEngine::onEditCapturedVoiceDataEvent`

This is pre-encode, which is why the receive-side radio DSP applies to the
ambience automatically, exactly as it does to voice.

**`*edited |= 1` is load-bearing.** TeamSpeak silently discards modified samples
without it, which presents as broken capture while the mix works perfectly.

## Gating — no SQF layer

Capture is hooked to the extension's own internal transmit-state signals rather
than polling `acre_api_fnc_isBroadcasting` from an addon:

- `extensions/src/ACRE2Core/startRadioSpeaking.h` → start capture
- `extensions/src/ACRE2Core/stopRadioSpeaking.h` → stop and flush

This is the single most important performance decision: capture only runs during
actual transmission windows, which are few and short regardless of how intense
combat is elsewhere on the server.

## Design constraints, and how each resolved

1. **Dedicated capture thread feeding a lock-free ring buffer**, drained without
   blocking by the audio callback. Single-producer/single-consumer, no locks,
   because blocking the audio path on a thread waiting for a socket or an audio
   device risks audible glitches in the player's own voice. Bounded latency is
   the consumer's job: it discards its own backlog rather than playing catch-up
   through stale audio, since ambience that lags the speech it accompanies is
   worse than no ambience.
2. **Format conversion** — resolved to nothing. Both backends deliver 48 kHz
   mono s16 directly. The Windows client accepts a caller-chosen format and the
   Linux helper converts before the wire, so no resampler exists.
3. **PID targeting.** Arma is found by executable name. Two-instance test setups
   need explicit disambiguation and are warned about; normal play does not.
4. **Amplitude noise gate**, applied before mixing — deliberately *not* a
   content classifier. Suppresses the quiet bed (foliage, waves, footsteps) that
   would otherwise sit under every transmission. −35 dBFS with a 200 ms hold;
   the hold is not a refinement, without it combat produces audible chattering.
5. **Non-diegetic content (music, UI) is handled by instructing the player to
   zero those Arma sliders**, not by post-capture filtering. Verified: zeroing
   silences completely rather than attenuating.
6. **Own-gunfire dominance is expected and acceptable.** Not a bug. Heavy rain
   passing the gate is the same principle — a real operator transmitting in a
   rainstorm would sound like it.

## Realism is a heuristic here, not a goal

The ambient chain borrows real mechanisms -- a noise-cancelling microphone
model, a VOGAD-style compressor -- but it is sound design, not simulation, and
the documentation should not let the vocabulary imply otherwise. The high-pass
is twice as steep as a real gradient mic and has no rejection floor; the
compressor sits on ambience alone where a real VOGAD sits on the combined mic
signal; the noise gate silences ambience while voice continues, which one
microphone cannot do; and the whole two-bus architecture replaces a ratio that
physics would otherwise set.

This is deliberate. Realism earned its place by producing the single most
valuable change -- the mic model, which turned out to be removing energy the
receive filter discards anyway -- and lost an argument on measurement when the
authentic slow VOGAD release proved worse for keeping a quiet bed audible.

**The rule: realism is a source of ideas, not of values.** Take the idea,
measure the result, and keep whichever setting sounds better. `ambient/README.md`
carries the full audit and the ranked list of what would actually move closer.

## Deferred — do not build without asking

- **Supersonic crack / near-miss audio.** Out of scope entirely.
- **Nearby direct-voice bleed.** If revisited: tap ACRE2's per-speaker channel
  list (`updateSpeakingData.h`, `Speaking` enum in `ACRE2Shared/Types.h`), filter
  for `speakingType == direct` (never `radio`, to avoid a feedback loop), and
  apply a steep attenuation curve plus a loudness gate so only shouting passes.
  **Flag to the user before building:** this broadcasts a real bystander's live
  microphone audio without their knowledge. Confirm whether a minimum-duration
  gate is wanted, so single sharp real-world noises do not leak.
- **Ducking / AGC on the combined signal.** Undecided. Either add a compression
  stage before the existing hard clip in `FilterRadio.cpp`, or accept that clip
  as realistic cheap-radio overload behaviour. Becomes relevant only if a real
  listener reports the mix clipping unpleasantly.

## Alternative design, if the current one had failed

A discrete fired-event / canned-sample architecture was separately researched
and validated as feasible: a `"FiredNear"` event handler, a weapon-category
sample library, and mixing-pool channels. It is a **full alternative, not an
extension** of this design. It was never needed — the go/no-go gate passed — but
if the capture approach ever has to be abandoned, that is the fallback rather
than a hybrid.

## Environment

**Development and play is Linux**, which upstream does not support. Arma and
TeamSpeak both run under Proton in Arma's own prefix. The plugin cross-compiles
with mingw-w64; upstream's MSVC path is preserved and checked by CI.

**The target is Windows**, where the WASAPI backend is the one that runs. It
cannot execute under Proton or on CI runners (no audio endpoint), so a local
Windows VM exists for it. See `ambient/README.md`.

## Distribution

Any distributed build, even friends-only, triggers GPLv3 source availability.
The fork is public, so pointing at the repository satisfies this.

Note that ACRE2 imports `X3DAudio1_7.dll` from the legacy DirectX runtime —
stock upstream does too, so anyone already running ACRE2 has it, but a clean
machine reports only "Failed to load plugin" with no indication why.
