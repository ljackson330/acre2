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

## Open: is the noise gate (requirement 4) needed?

Undecided. Level distribution over `fight` in 50 ms windows:

```
p5    −55.3 dBFS      p50   −26.9 dBFS      p99   −17.8 dBFS
```

The −55.3 dBFS floor looks low enough to make a gate pointless — but this take
is 30s of *continuous* firefight (p25 is already −34 dBFS), so it contains no
idle. The gate's job is suppressing residue during quiet transmissions, and that
condition is unmeasured.

**Needs one more take:** 30s standing or walking somewhere quiet, sliders zeroed,
no combat. Floor near −55 dBFS → drop the gate from Phase 2. Floor near −35 dBFS
→ build it.

## Take 5 (isolation) — not run

Skipped: no second player available, and received radio audio is known to come
through the TS3 node, not Arma's. That is the same process-separation property
the PID-targeted design relies on, and it gets proven for free the first time
Tier A runs.
