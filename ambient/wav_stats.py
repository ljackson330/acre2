#!/usr/bin/env python3
"""Report objective level stats for a Phase 0 capture.

Answers the question the gameplan actually cares about: when the player zeroes
Arma's music/UI sliders, is the captured content *silent* or merely attenuated?
"True silence" means a digital-zero floor, not a quiet-but-present signal.
"""
import os
import struct
import subprocess
import sys
import tempfile
import wave


def dbfs(x, full):
    return "-inf" if x <= 0 else f"{20.0 * __import__('math').log10(x / full):.1f} dBFS"


def main(path, bucket=1.0):
    # Phase 0 takes are archived as FLAC (lossless); decode to a temp WAV first.
    orig = path
    tmp = None
    if path.lower().endswith(".flac"):
        tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        tmp.close()
        subprocess.run(["flac", "-s", "-d", "-f", path, "-o", tmp.name], check=True)
        path, cleanup = tmp.name, tmp.name
    else:
        cleanup = None

    w = wave.open(path)
    ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    if width != 2:
        sys.exit(f"expected 16-bit PCM, got {width * 8}-bit")
    full = 32768.0
    raw = w.readframes(n)
    s = struct.unpack(f"<{len(raw) // 2}h", raw)
    if cleanup:
        os.unlink(cleanup)

    print(f"file      {orig}")
    print(f"format    {rate} Hz, {ch} ch, {width * 8}-bit, {n / rate:.2f}s ({n} frames)")
    if not n:
        sys.exit("no frames captured -- stream was corked or target wrong")

    peak = max(abs(v) for v in s)
    rms = (sum(v * v for v in s) / len(s)) ** 0.5
    print(f"peak      {peak:6d}  {dbfs(peak, full)}")
    print(f"rms       {rms:9.1f}  {dbfs(rms, full)}")
    nz = sum(1 for v in s if v != 0)
    print(f"non-zero  {nz}/{len(s)} samples ({100.0 * nz / len(s):.2f}%)")
    print()
    if peak == 0:
        print("VERDICT: digital silence. Content is genuinely absent from the capture.")
    elif peak < 32:  # ~-60 dBFS
        print("VERDICT: not silent, but below -60 dBFS -- attenuated, not removed.")
        print("         A noise gate would suppress this. Confirm by ear.")
    else:
        print("VERDICT: clearly present. Content is NOT silenced by the settings tested.")

    # Per-bucket breakdown so a short sound in a long take is not averaged away.
    step = int(rate * bucket) * ch
    if len(s) > step:
        print(f"\nper-{bucket:g}s peak:")
        for i in range(0, len(s) - step + 1, step):
            b = s[i:i + step]
            p = max(abs(v) for v in b)
            bar = "#" * int(40 * p / full)
            print(f"  {i / ch / rate:5.1f}s  {p:6d}  {bar}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: wav_stats.py <file.wav|file.flac> [bucket_seconds]")
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 1.0)
