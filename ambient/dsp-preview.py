#!/usr/bin/env python3
"""Apply the ambient DSP chain to a file, so parameters can be heard offline.

Use this on **pure ambience** -- the Phase 0 takes (ambient/fight.flac,
idle.flac, idle2.flac) or anything recorded straight from Arma's node. Those are
what the chain actually sees in production.

Running it on an `ambientDumpFile` recording will not tell you much and can
mislead: a dump contains voice summed with ambience and has already been through
the receive-side radio filter, which high-passes at 750 Hz. The mic model will
appear to do nothing, because the energy it removes has already been removed,
and the compressor will duck the voice, which it never does in production.

To hear the chain on your own gameplay, run it live in ambient-helper.py
instead -- the helper carries ambience and nothing else, so it is the honest
place to try this without rebuilding the plugin.

    ./ambient/dsp-preview.py ambient/fight.flac --dsp -o /tmp/fight-dsp.wav
    ./ambient/dsp-preview.py ambient/fight.flac --dsp --compare -o /tmp/ab.wav
"""

import argparse
import math
import pathlib
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ambient_dsp import add_dsp_arguments, chain_from_args  # noqa: E402

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required: pip install numpy")


def load(path):
    path = str(path)
    tmp = None
    if path.lower().endswith(".flac"):
        tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        subprocess.run(["flac", "-s", "-d", "-f", path, "-o", tmp.name], check=True)
        path = tmp.name
    with wave.open(path) as w:
        if w.getsampwidth() != 2:
            sys.exit("only 16-bit PCM is supported")
        rate, channels = w.getframerate(), w.getnchannels()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    if tmp:
        pathlib.Path(tmp.name).unlink(missing_ok=True)
    if channels > 1:
        # The capture path is mono by the time the chain sees it.
        data = data.reshape(-1, channels).mean(axis=1).astype(np.int16)
    return data, rate


def write(path, samples, rate):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.astype("<i2").tobytes())


def stats(samples):
    if len(samples) == 0:
        return (-999.0, -999.0)
    f = samples.astype(np.float64)
    peak = float(np.abs(f).max())
    rms = float(np.sqrt((f * f).mean()))
    to_db = lambda v: 20.0 * math.log10(v / 32768.0) if v > 0 else -999.0
    return to_db(peak), to_db(rms)


def window_rms_db(samples, window=480):
    f = samples.astype(np.float64)
    usable = (len(f) // window) * window
    if usable == 0:
        return np.array([])
    blocks = f[:usable].reshape(-1, window)
    rms = np.sqrt((blocks * blocks).mean(axis=1))
    return 20.0 * np.log10(np.maximum(rms, 1e-9) / 32768.0)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="wav or flac of pure ambience")
    ap.add_argument("-o", "--output", help="write processed audio here")
    ap.add_argument("--compare", action="store_true",
                    help="output dry for 4 s, then wet, alternating, for A/B by ear")
    add_dsp_arguments(ap)
    args = ap.parse_args()

    dry, rate = load(args.input)
    # The preview is for hearing the chain; default it on rather than making
    # --dsp mandatory noise on every invocation.
    args.dsp = True
    chain = chain_from_args(args, rate)

    print(f"input : {args.input}")
    print(f"        {len(dry)/rate:.2f} s, {rate} Hz")
    print(f"chain : {chain.describe()}")

    wet = chain.process_int16(dry)

    dry_peak, dry_rms = stats(dry)
    wet_peak, wet_rms = stats(wet)
    print()
    print(f"{'':<10}{'peak':>10}{'rms':>10}{'crest':>10}")
    print(f"{'dry':<10}{dry_peak:>10.1f}{dry_rms:>10.1f}{dry_peak-dry_rms:>10.1f}")
    print(f"{'wet':<10}{wet_peak:>10.1f}{wet_rms:>10.1f}{wet_peak-wet_rms:>10.1f}")
    print(f"{'change':<10}{wet_peak-dry_peak:>+10.1f}{wet_rms-dry_rms:>+10.1f}"
          f"{(wet_peak-wet_rms)-(dry_peak-dry_rms):>+10.1f}")

    if chain.compressor is not None:
        print(f"\nmax gain reduction: {chain.compressor.max_reduction_db:.1f} dB")

    # The point of the exercise is the spread between quiet bed and loud events,
    # so report it directly rather than leaving it to be inferred from crest.
    d_win, w_win = window_rms_db(dry), window_rms_db(wet)
    if len(d_win):
        print(f"\n{'':<10}{'p10':>8}{'p50':>8}{'p95':>8}{'p99':>8}{'p99-p50':>10}")
        for name, win in (("dry", d_win), ("wet", w_win)):
            p = lambda q: float(np.percentile(win, q))
            print(f"{name:<10}{p(10):>8.1f}{p(50):>8.1f}{p(95):>8.1f}{p(99):>8.1f}"
                  f"{p(99)-p(50):>10.1f}")
        print("\np99-p50 is the number this is all about: how far the loud events")
        print("sit above the typical bed. Smaller means the rotor wash survives")
        print("a level cut that the gunfire also has to survive.")

    if args.output:
        if args.compare:
            # Alternating blocks make the difference obvious by ear in a way
            # two separate files never do.
            block = rate * 4
            pieces, pos, use_wet = [], 0, False
            while pos < len(dry):
                src = wet if use_wet else dry
                pieces.append(src[pos:pos + block])
                pos += block
                use_wet = not use_wet
            out = np.concatenate(pieces)
            print(f"\nwrote {args.output} -- alternating 4 s dry/wet, starting dry")
        else:
            out = wet
            print(f"\nwrote {args.output}")
        write(args.output, out, rate)


if __name__ == "__main__":
    main()
