# ACRE2 Ambient Battle Sound — Implementation Spec (Process-Loopback Approach)

Fork https://github.com/IDI-Systems/acre2 (GPLv3, Arma 3). Goal: get it
working, not upstream-ready. Private/local fork, no need for clean
rebase-ability against upstream `master`.

## Feature
When a player transmits on an ACRE2 radio, capture their own Arma 3 process
audio output live and mix it into the outgoing transmission, so listeners
hear the transmitter's real combat environment (gunfire, explosions,
engines) riding through the normal TS3 voice pipeline.

## Mechanism
Windows `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` via
`ActivateAudioInterfaceAsync` (Windows 10 2004+/Build 19041+). Reference:
Microsoft's `ApplicationLoopback` sample. Captures raw PCM output of a
specific process by PID.

**Hard requirement:** scope capture strictly to the local `Arma3.exe`
process. Never full-device/system loopback — ACRE2's received-radio audio
plays back through the TS3 client process (separate from Arma3.exe), so
system-wide loopback risks capturing and re-transmitting radio audio this
client is already receiving.

## Injection point (unchanged from prior research)
- `extensions/src/ACRE2TS/TsCallbacks_sound.cpp` →
  `ts3plugin_onEditCapturedVoiceDataEvent`
- Implementation: `extensions/src/ACRE2Core/SoundEngine.cpp` →
  `CSoundEngine::onEditCapturedVoiceDataEvent` (currently near-stub). Sum
  the capture ring buffer into the outgoing mic buffer here, pre-encode.

## Gating — no SQF layer needed
Do NOT build an SQF detection addon or poll `acre_api_fnc_isBroadcasting`.
Instead hook the extension's own internal transmit-state signals directly:
- `extensions/src/ACRE2Core/startRadioSpeaking.h` → start capture thread
- `extensions/src/ACRE2Core/stopRadioSpeaking.h` → stop + flush capture thread

This is the single most important performance decision: capture only runs
during actual transmission windows, which are few and short regardless of
combat intensity elsewhere on the server.

## Architecture requirements
1. **Dedicated capture thread** reading the WASAPI loopback client, feeding
   a thread-safe ring buffer. `onEditCapturedVoiceDataEvent` drains it
   without blocking. Handle over/underrun, clean start/stop tied to the
   hooks above, and Arma/TS3 disconnect edge cases explicitly.
2. **Resampling/format conversion.** Arma's render output format (verify —
   likely 48kHz float) will likely not match TS3's expected capture format.
   Confirm and build a converter stage.
3. **PID targeting.** Find the local `Arma3.exe`/`arma3_x64.exe` process.
   Note: two-Arma-instance test setups (see Testing section) require
   explicit disambiguation logic; normal single-instance play does not.
4. **Noise gate.** Simple amplitude-threshold gate on the ring buffer,
   applied before mixing — suppresses near-silent residue (distant
   footsteps/rustling already attenuated by Arma's own 3D mix). Not a
   content classifier — no attempt to distinguish sound types.
5. **Non-diegetic content (music, UI) is handled by instructing the player
   to zero those Arma audio-settings sliders**, not by post-capture
   filtering. Verify in Phase 0 that zeroing actually silences the content
   in the captured stream (not just attenuates it), and confirm exactly
   which categories Arma's settings expose.
6. **Own-gunfire dominance is expected and acceptable** — do not treat a
   transmitter's own weapon audio dominating the mix as a bug.

## Build order

**Phase 0 (do first, before any fork code):** Standalone C++ test program,
separate from ACRE2, using `ActivateAudioInterfaceAsync` +
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` to capture local Arma3.exe
output to a WAV file. Verify: music/UI sliders zeroed → actually silent in
capture; gunfire/explosions/engines recognizable in a recorded AI firefight;
note actual sample rate/format produced; assess whether the noise gate is
even necessary from this recording. **This is the highest-uncertainty
assumption in the whole design — stop and reassess if this doesn't sound
usable, rather than proceeding to Phase 2.**

**Phase 1:** Confirm Windows SDK version supports the needed
`AUDIOCLIENT_ACTIVATION_PARAMS` struct. Do a due-diligence check on
BattlEye/EAC compatibility with process-loopback capture before proceeding
— this hasn't been verified, only reasoned about as low-risk (sanctioned OS
API, no memory read/write or code injection into Arma's process).

**Phase 2:** Port Phase 0 capture code into the extension as described
under Architecture requirements above (thread + ring buffer, hooked to
start/stopRadioSpeaking, PID targeting, resampling, noise gate). Verify via
extension logs: correct start/stop alignment with transmit state, no
crashes/leaks across repeated cycles, no buffer over/underrun under normal
use.

**Phase 3:** Wire into `onEditCapturedVoiceDataEvent`. Test via:
- **Tier A (solo):** enable TS3's built-in local mic-monitor/"Sound Check"
  playback; confirm you hear the injected mix at the source while
  transmitting.
- **Tier B (two-client, full validation):** two Steam/Arma accounts + two
  TS3 identities on one machine, both audio outputs audible. Instance A =
  transmitter near AI combat; instance B = listener elsewhere in radio
  range, out of earshot of the real gunfire. Confirm PID targeting isolates
  instance A only.

**Phase 4 (tuning):** Confirm own-gunfire dominance matches expectations;
tune noise-gate threshold against real Tier B results; confirm
receive-side radio DSP (`FilterRadio.cpp`/`RadioEffect.cpp` — bandpass
750-4000Hz, noise, distortion, hard clip) is correctly applying to injected
content once received (it should, automatically, given the injection
point — confirm by ear).

## Deferred, do not build yet
- **Supersonic crack/near-miss audio** — out of scope entirely for now.
- **Nearby direct-voice bleed** — follow-on after core capture is proven,
  not part of Phases 0-4. If revisited: tap ACRE2's internal per-speaker
  channel list (`extensions/src/ACRE2Core/updateSpeakingData.h`,
  `Speaking` enum in `ACRE2Shared/Types.h`), filter for
  `speakingType == direct` (never `radio`, to avoid feedback-loop risk),
  apply a custom steep attenuation curve + short-term-energy loudness gate
  so only shouting passes through. Flag to the user before building: this
  broadcasts a real bystander's live mic audio without their knowledge;
  confirm whether a minimum-duration gate (avoid single sharp real-world
  noises leaking) is wanted before implementing.
- **Ducking/AGC on the combined signal** — undecided; either build a
  compression stage before the existing hard-clip in `FilterRadio.cpp`, or
  accept the existing hard-clip/distortion behavior as-is (itself a
  realistic cheap-radio-overload behavior). Ask before choosing if not
  specified.

## If Phase 0 fails
Do not attempt to patch this design into something else. A discrete
fired-event/canned-sample architecture was separately researched and
validated as feasible (different injection strategy: `"FiredNear"` event
handler + weapon-category sample library + mixing-pool channels) but is a
full alternative design, not an extension of this one. Flag to the user
rather than improvising a hybrid.

## Environment
Windows required (extension has hard dependencies: `Wave.h` includes
`<Windows.h>`; `FilterPosition.cpp` links XAudio2 — not portable to Linux
without a real rewrite). Toolchain: HEMTT (SQF/PBO side, minimal use in
this design), CMake + Visual Studio 2017+ + DirectX SDK (extension side).
If working from Linux: build via GitHub Actions Windows runner or a
throwaway cloud Windows instance; test via Arma 3 on Proton + TeamSpeak's
**Windows** client under Wine (not native Linux TS3 client — it can't load
a Windows-compiled plugin `.dll`). Verify stock ACRE2 works end-to-end
under this setup, including mic capture, before trusting it as the dev
loop; dual-boot Windows is the fallback if Wine audio capture proves
unreliable.

## Distribution
Any distributed build (even friends-only) triggers GPLv3 source-
availability — be prepared to share source alongside any build handed out.
No obligation to publish publicly for private testing.