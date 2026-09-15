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
