"""Ambient-bus DSP: a noise-cancelling mic model and a VOGAD-style compressor.

Both stages belong on the *ambience only*, never on the summed signal, which is
why they live here in the helper rather than in the plugin: the helper carries
Arma's audio and nothing else, so anything applied here cannot touch the
player's voice. It also means parameters can be tuned by restarting a Python
process instead of rebuilding a DLL and TeamSpeak.

Why a high-pass, and why it matters more than it looks
------------------------------------------------------
Real tactical and aviation headsets use noise-cancelling pressure-gradient
microphones. They sense the pressure *difference* across two closely spaced
ports: the mouth, an inch away, produces a steep gradient and passes; the far
field arrives at both ports almost equally and largely cancels. That
cancellation is strongest at low frequencies, where the wavelength dwarfs the
port spacing, and weakens as frequency rises. So ambient sound reaching a real
radio is high-pass filtered -- the rumble goes, the crack stays.

Measured against the Phase 0 takes, 76% of a firefight's energy and 93% of heavy
rain's sits below 300 Hz. Meanwhile ACRE2's receive-side radio filter high-passes
at 750 Hz, so **that energy is discarded before anyone hears it**. Until then it
is eating headroom at the mix, driving FilterRadio's 3x boost into foldback, and
clipping. Removing it is close to inaudible in the result and buys back a great
deal of room.

Why a compressor, and why it is not just a volume control
---------------------------------------------------------
Turning the ambience down is a linear scalar: it cannot tell a gunshot from
rotor wash, so the quiet bed disappears long before the transients stop being
obnoxious. A compressor narrows the distance between them instead, leaving the
overall level to be set separately.

The realistic version of this is a VOGAD -- the Voice-Operated Gain Adjusting
Device in essentially every military radio -- which has a fast attack and a slow
release. The authentic artifact is that a loud event pulls the gain down and it
walks back up over the following second. Pumping is a defect in music
production; here it is the sound of transmitting next to something violent.

What the measurements actually showed
-------------------------------------
Swept against ambient/fight.flac, using the spread between the median window
and the 99th percentile -- how far the loud events sit above the typical bed,
which is the number this whole stage exists to reduce:

After the high-pass the bed sits at -34.3 dBFS and the loud events at -21.6,
so the threshold belongs between them -- below the bed it is only an attenuator
with extra steps. Holding threshold -28, ratio 8:1, attack 1 ms and sweeping
release, measuring how far each moves the bed versus the loud events:

    release     bed      loud    spread
    (none)      ----     ----     12.8 dB
     40 ms     -2.0 dB  -6.9 dB    7.8 dB
     80 ms     -3.1     -7.4       8.5
    150 ms     -4.5     -8.2       9.1
    300 ms     -5.8     -9.0       9.6
    600 ms     -7.1     -9.6      10.3

Monotonic, and it settles the VOGAD question. A slow, realistic release is
measurably *worse* for this: sustained fire never gives it time to recover, so
it holds the gain down through the quiet parts and ducks the bed by nearly as
much as the gunshot. 40 ms leaves the bed almost intact while still taking 7 dB
off the loud events.

The high-pass, separately, buys headroom rather than dynamics -- about 6 dB of
RMS for no change in spread at all. Both stages are worth having; they are just
solving different problems.
"""

import math

try:
    import numpy as np
except ImportError:  # the helper works fine without DSP
    np = None


class Biquad:
    """Direct Form I biquad, holding state across chunks so a streaming caller
    gets the same result as processing the whole file at once."""

    def __init__(self, b0, b1, b2, a1, a2):
        self.b0, self.b1, self.b2, self.a1, self.a2 = b0, b1, b2, a1, a2
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    @classmethod
    def highpass(cls, sample_rate, cutoff_hz, q=0.707):
        w0 = 2.0 * math.pi * cutoff_hz / sample_rate
        cos_w0, sin_w0 = math.cos(w0), math.sin(w0)
        alpha = sin_w0 / (2.0 * q)

        b0 = (1.0 + cos_w0) / 2.0
        b1 = -(1.0 + cos_w0)
        b2 = (1.0 + cos_w0) / 2.0
        a0 = 1.0 + alpha
        a1 = -2.0 * cos_w0
        a2 = 1.0 - alpha
        return cls(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)

    def process(self, samples):
        b0, b1, b2, a1, a2 = self.b0, self.b1, self.b2, self.a1, self.a2
        x1, x2, y1, y2 = self.x1, self.x2, self.y1, self.y2
        out = samples.copy()
        for i in range(len(samples)):
            x0 = samples[i]
            y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            out[i] = y0
            x2, x1 = x1, x0
            y2, y1 = y1, y0
        self.x1, self.x2, self.y1, self.y2 = x1, x2, y1, y2
        return out


class Compressor:
    """Feed-forward peak compressor with a soft knee, operating in dB.

    Gain reduction is smoothed rather than the signal level, so the attack and
    release times mean what they say regardless of programme material.
    """

    def __init__(self, sample_rate, threshold_db=-28.0, ratio=8.0,
                 attack_ms=1.0, release_ms=40.0, knee_db=6.0, makeup_db=0.0):
        self.threshold_db = threshold_db
        self.ratio = ratio
        self.attack_ms = attack_ms
        self.release_ms = release_ms
        self.knee_db = knee_db
        self.makeup = 10.0 ** (makeup_db / 20.0)
        # One-pole coefficients. A gunshot needs a few ms to be caught without
        # flattening the initial crack into a thud; the release is what decides
        # whether the bed breathes back in or pumps.
        self.attack = math.exp(-1.0 / (max(attack_ms, 0.01) * 0.001 * sample_rate))
        self.release = math.exp(-1.0 / (max(release_ms, 0.01) * 0.001 * sample_rate))
        self.envelope_db = 0.0   # current gain reduction, <= 0
        self.max_reduction_db = 0.0

    def _target_reduction(self, level_db):
        over = level_db - self.threshold_db
        half_knee = self.knee_db / 2.0
        if over <= -half_knee:
            return 0.0
        if over >= half_knee and self.knee_db > 0.0:
            return -over * (1.0 - 1.0 / self.ratio)
        if self.knee_db <= 0.0:
            return -over * (1.0 - 1.0 / self.ratio)
        # Quadratic soft knee: the curve meets the hard-knee line at both edges.
        x = over + half_knee
        return -(1.0 - 1.0 / self.ratio) * (x * x) / (2.0 * self.knee_db)

    def process(self, samples):
        out = samples.copy()
        env = self.envelope_db
        for i in range(len(samples)):
            level = abs(samples[i]) / 32768.0
            level_db = 20.0 * math.log10(level) if level > 1e-9 else -180.0
            target = self._target_reduction(level_db)
            # Attack when clamping down harder, release when letting go.
            coeff = self.attack if target < env else self.release
            env = target + coeff * (env - target)
            if env < self.max_reduction_db:
                self.max_reduction_db = env
            out[i] = samples[i] * (10.0 ** (env / 20.0)) * self.makeup
        self.envelope_db = env
        return out


class AmbientChain:
    """mic model -> dynamics -> level, in that order.

    The high-pass runs first so the compressor is not triggered by low-frequency
    energy that the receive-side radio filter is going to discard anyway --
    otherwise rumble ducks the whole bed for sounds nobody ever hears.
    """

    def __init__(self, sample_rate, highpass_hz=300.0, threshold_db=-28.0,
                 ratio=8.0, attack_ms=1.0, release_ms=40.0, makeup_db=0.0,
                 gain_db=0.0, enable_highpass=True, enable_compressor=True):
        self.highpass = (Biquad.highpass(sample_rate, highpass_hz)
                         if enable_highpass and highpass_hz > 0 else None)
        self.compressor = (Compressor(sample_rate, threshold_db, ratio,
                                      attack_ms, release_ms, makeup_db=makeup_db)
                           if enable_compressor else None)
        self.gain = 10.0 ** (gain_db / 20.0)

    def process_int16(self, pcm):
        """pcm: numpy int16 array. Returns int16, clipped."""
        if np is None:
            return pcm
        work = pcm.astype(np.float64)
        if self.highpass is not None:
            work = self.highpass.process(work)
        if self.compressor is not None:
            work = self.compressor.process(work)
        if self.gain != 1.0:
            work *= self.gain
        np.clip(work, -32768, 32767, out=work)
        return work.astype(np.int16)

    def describe(self):
        bits = []
        if self.highpass is not None:
            bits.append("high-pass")
        if self.compressor is not None:
            c = self.compressor
            bits.append(f"compressor {c.threshold_db:.0f}dB {c.ratio:.1f}:1 "
                        f"a{c.attack_ms:g}ms r{c.release_ms:g}ms")
            if c.makeup != 1.0:
                bits.append(f"makeup {20*math.log10(c.makeup):+.1f}dB")
        if self.gain != 1.0:
            bits.append(f"gain {20*math.log10(self.gain):+.1f}dB")
        return ", ".join(bits) if bits else "passthrough"


def add_dsp_arguments(parser):
    """Shared CLI surface, so the helper and the offline preview cannot drift."""
    g = parser.add_argument_group("ambient DSP")
    g.add_argument("--dsp", action="store_true",
                   help="enable the ambient DSP chain (off by default)")
    g.add_argument("--no-highpass", action="store_true",
                   help="skip the noise-cancelling mic model")
    g.add_argument("--no-compressor", action="store_true",
                   help="skip the VOGAD-style compressor")
    g.add_argument("--highpass-hz", type=float, default=300.0,
                   help="mic model cutoff in Hz (default: 300)")
    g.add_argument("--threshold", type=float, default=-28.0,
                   help="compressor threshold in dBFS; must sit above the quiet "
                        "bed and below the loud events, or it is only an "
                        "attenuator with extra steps (default: -28)")
    g.add_argument("--ratio", type=float, default=8.0,
                   help="compressor ratio (default: 8)")
    g.add_argument("--attack", type=float, default=1.0,
                   help="attack in ms; 1 tames gunfire best, raise toward 5 to "
                        "keep more of the crack (default: 1)")
    g.add_argument("--release", type=float, default=40.0,
                   help="release in ms; shorter is strictly better here, because "
                        "sustained fire never lets a long release recover and it "
                        "ends up ducking the bed too (default: 40)")
    g.add_argument("--makeup", type=float, default=0.0,
                   help="makeup gain in dB (default: 0 -- level belongs in acre2.ini)")
    g.add_argument("--gain", type=float, default=0.0,
                   help="extra gain in dB applied after everything (default: 0)")
    return parser


def chain_from_args(args, sample_rate):
    return AmbientChain(
        sample_rate,
        highpass_hz=args.highpass_hz,
        threshold_db=args.threshold,
        ratio=args.ratio,
        attack_ms=args.attack,
        release_ms=args.release,
        makeup_db=args.makeup,
        gain_db=args.gain,
        enable_highpass=not args.no_highpass,
        enable_compressor=not args.no_compressor,
    )
