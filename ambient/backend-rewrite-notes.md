# If the backend is ever replaced

Notes from a 2026-09-17 design conversation. **Nothing here is committed to**,
and nothing has been prototyped. It exists so the research does not have to be
redone.

The trigger was the proximity-chat leak (see the open question in
[README.md](README.md)): mixing ambience at the transmitter, pre-encode, puts it
on the bystanders' direct-speech channel as well as the radio net. Every fix
needs code on the receiving end, which **Liam has confirmed is acceptable — all
listeners will run the fork.** That reopens designs that were previously ruled
out.

## What is actually worth keeping

The addon layer is the mod. 1044 SQF files, ~50k lines, and the parts that
matter most are the least replaceable:

- `sys_signal` / `sys_antenna` — real RF propagation, terrain and climate codes,
  signal quality per link.
- `sys_rack`, `sys_intercom`, `sys_attenuate` — vehicle racks, intercom,
  environmental attenuation.
- Per-radio UIs and models for ten-plus real radios.

All of it is transport-agnostic. **A backend rewrite replaces the TeamSpeak
plugin and the transport under it, not the mod.**

## Asset licensing — checked, and the answer is permissive

The root `LICENSE` is GPLv3 and `README.md` notes that some folders carve
themselves out. Every carve-out, enumerated:

| path | restriction |
|---|---|
| `addons/compat_csla` | `.fakewrp` terrain stubs — CSLA Studio |
| `addons/compat_gm` | `.fakewrp` — Vertexmacht |
| `addons/compat_sog` | `.fakewrp` — Savage Game Design |
| `addons/compat_spe` | `.fakewrp` — Heavy Ordnance Works |
| `addons/compat_ws` | `.fakewrp` — Rotators Collective |
| `addons/sys_intercom/vic3/data` | RanTa |

Those are terrain stubs and one intercom data folder. **Everything else is
GPLv3**, including all 14 `.p3d` radio models (PRC-152, 148, 343, 117F,
SEM52SL, SEM70, PRC-77, BF888S, GSA VHF-30108, OE303), all 921 `.paa` textures,
the fonts and the UI.

Reuse in a differently-named mod is therefore fine, with three conditions:

1. the new mod is **GPLv3**;
2. **IDI-Systems is credited** and the licence text travels with it;
3. the **ACRE name and logo are dropped** — trademark is separate from
   copyright, and `acre_logo_medium_ca.paa` is branding.

GPLv3's "preferred form for modification" is awkward for binarized `.p3d`, but
it does not bite as long as the assets are redistributed unmodified.

## The transport options, cheapest first

**A — mix at the transmitter (today).** Stock listeners work. Bleeds to
proximity chat; one ambience treatment for everyone.

**B — ambience over TeamSpeak plugin commands, mixed at the receiver.** The
near-term fix. Costs libopus, base64, ~250 ms batching, a per-speaker jitter
buffer. **Gated on a flood-limit spike**, which is solo-testable and can kill
the approach outright — read the server's
`virtualserver_antiflood_points_*` settings first if there is admin access.
Payload is smaller than it first appears: `FilterRadio` band-passes 750 Hz to
4 kHz (`FilterRadio.cpp:93-94`), so encoding at radio bandwidth (~8-12 kbps)
wastes nothing the listener would have heard.

**C — synthesize ambience at the receiver from game events.** gameplan.md's
documented fallback. Deletes both capture backends, gives up real captured
audio, needs a sample library.

**D — leave TeamSpeak entirely: fork a voice client.** The ground-up option.

### D, in more detail

Being a plugin inside a proprietary client is the root cause of most of the
friction: one voice stream per client (hence the leak), `*edited |= 1`, plugin
command flood limits, MSVC-era build workarounds, and a client whose successors
have a different plugin API.

**Fork a voice stack rather than writing one.**
[Mumble](https://github.com/mumble-voip/mumble) is BSD-3-Clause, Qt and Opus,
with native Linux and Windows clients and its own server — permissive enough to
sit under a GPLv3 addon layer without friction. A fork can add message types and
put per-link DSP directly in the audio pipeline.
[DCS-SimpleRadioStandalone](https://github.com/ciribob/DCS-SimpleRadioStandalone)
is the prior art for the standalone-client pattern.

What it unlocks:

- ambience as **its own stream**, so the leak cannot exist;
- **per-listener, per-link DSP** driven by the signal quality `sys_signal`
  already computes — noise floor, squelch tails, multipath, vocoder artifacts;
- **one codebase for both platforms**, which is the only option that genuinely
  removes the WASAPI/PipeWire split. The existing Linux helper already proves
  the native-app-talks-to-the-game pattern.

What it costs:

- someone **hosts a voice server** (incremental — Arma and TeamSpeak are already
  hosted);
- every player **swaps TeamSpeak for a custom client**;
- **positional audio needs reimplementing** — X3DAudio lives in the plugin
  today, though the position data already arrives from SQF;
- a **Mumble fork to maintain**, indefinitely.

Months, not weeks. It prototypes independently of the running fork: two
machines, push-to-talk, radio DSP, ambience as a separate stream.

## Sequencing, if any of this is picked up

1. **Measure the echo** (Test A, bystander with OBS). It may show the leak is
   inaudible, in which case none of this is needed.
2. **Run the flood spike** — solo, an evening, and it decides whether B exists.
3. Only then choose. B is a few weeks; D is a different project, and the DSP
   work carries into both.
