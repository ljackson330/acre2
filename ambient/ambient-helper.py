#!/usr/bin/env python3
"""Ambient battle sound capture helper (Linux/PipeWire side).

The ACRE2 TeamSpeak plugin runs as a Windows DLL inside Wine and cannot call
PipeWire directly. This helper runs natively, captures Arma's audio node, and
serves it over a loopback socket that the plugin reads with ordinary Winsock.

Capture is scoped to Arma's PipeWire node by process id, which is the Linux
equivalent of Windows AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK. It is
structurally incapable of picking up the TeamSpeak client's own output, which
is a separate node -- that is the gameplan's hard requirement, satisfied by
construction rather than by filtering.

The capture stream stays open whether or not the plugin is connected; samples
are discarded when nobody is listening. Starting pw-record per transmission
would put process spawn and PipeWire graph negotiation latency at the front of
every transmission, losing the first shot of a firefight -- precisely the audio
worth having. Idle cost is one read into a discard buffer.

Wire format: signed 16-bit mono at 48 kHz, which is exactly what TeamSpeak's
onEditCapturedVoiceDataEvent expects, so the plugin performs no conversion.

With --dsp it also applies the ambient-bus DSP chain (see ambient_dsp.py) before
sending. This stream carries Arma's audio and nothing else, so it is the correct
and only place that chain can run without touching the player's voice -- and
tuning it here needs a helper restart rather than a plugin rebuild and a
TeamSpeak restart.
"""

import argparse
import pathlib
import select
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ambient_dsp import add_dsp_arguments, chain_from_args

DEFAULT_PORT = 47806
RATE = 48000
CHANNELS = 1
CHUNK = 1920  # 960 int16 samples = 20 ms at 48 kHz


def log(msg):
    print(f"[ambient-helper] {msg}", flush=True)


def find_arma_node():
    """Return (serial, description) for Arma's PipeWire output node, or None.

    Under Proton both Arma and TeamSpeak report application.process.binary as
    wine64-preloader, so the binary name cannot tell them apart.
    application.name is the discriminator.
    """
    try:
        out = subprocess.run(["pactl", "list", "sink-inputs"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError) as e:
        log(f"pactl failed: {e}")
        return None

    serial = name = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("Sink Input #"):
            serial, name = line.split("#")[1], None
        elif line.startswith("object.serial ="):
            serial = line.split('"')[1]
        elif line.startswith("application.name ="):
            name = line.split('"')[1]
            # Match Arma, and never the TeamSpeak client we feed.
            if name and "arma" in name.lower() and "teamspeak" not in name.lower():
                return serial, name
    return None


def start_capture(serial, latency):
    cmd = ["pw-record", "--raw", "--target", str(serial),
           "--rate", str(RATE), "--channels", str(CHANNELS),
           "--format", "s16", "--latency", latency, "-"]
    log(f"capturing node {serial}: {' '.join(cmd)}")
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--target", help="PipeWire node serial (default: auto-detect Arma)")
    ap.add_argument("--latency", default="20ms")
    add_dsp_arguments(ap)
    args = ap.parse_args()

    chain = None
    if args.dsp:
        try:
            import numpy  # noqa: F401
        except ImportError:
            sys.exit("--dsp needs numpy: pip install numpy")
        chain = chain_from_args(args, RATE)
        log(f"ambient DSP enabled: {chain.describe()}")

    for tool in ("pw-record", "pactl"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found -- PipeWire tools are required")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(1)
    log(f"listening on 127.0.0.1:{args.port}")

    proc = None
    client = None
    sent = 0
    carry = b""     # odd trailing byte when a read splits a sample

    try:
        while True:
            # (Re)establish the capture stream. Arma may not be running yet, or
            # may have restarted with a new node.
            if proc is None or proc.poll() is not None:
                if proc is not None:
                    log("capture stream ended; rediscovering")
                found = (args.target, "manual") if args.target else find_arma_node()
                if not found:
                    time.sleep(2.0)
                    continue
                serial, name = found
                log(f"found Arma node: serial={serial} name={name!r}")
                proc = start_capture(serial, args.latency)

            watch = [srv, proc.stdout] + ([client] if client else [])
            ready, _, _ = select.select(watch, [], [], 1.0)

            for s in ready:
                if s is srv:
                    conn, _ = srv.accept()
                    if client:
                        log("second client rejected; one at a time")
                        conn.close()
                    else:
                        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        client, sent = conn, 0
                        log("plugin connected -- streaming")

                elif s is proc.stdout:
                    data = proc.stdout.read(CHUNK)
                    if not data:
                        proc.stdout.close()
                        proc.wait(timeout=2)
                        proc = None
                        break
                    if client:
                        if chain is not None:
                            # Filter state is continuous across chunks, so a
                            # sample must never be split across a process() call.
                            import numpy as np
                            buf = carry + data
                            usable = len(buf) - (len(buf) % 2)
                            carry = buf[usable:]
                            pcm = np.frombuffer(buf[:usable], dtype="<i2")
                            data = chain.process_int16(pcm).tobytes()
                        try:
                            client.sendall(data)
                            sent += len(data)
                        except OSError:
                            log(f"plugin disconnected after {sent / 2 / RATE:.1f}s of audio")
                            client.close()
                            client = None
                    # else: discard -- nobody listening

                elif s is client:
                    # The plugin never sends; readable means it closed. A hard
                    # reset raises rather than returning empty, so catch it.
                    try:
                        gone = not client.recv(1)
                    except OSError:
                        gone = True
                    if gone:
                        log(f"plugin disconnected after {sent / 2 / RATE:.1f}s of audio")
                        client.close()
                        client = None
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        if client:
            client.close()
        if proc and proc.poll() is None:
            proc.terminate()
        srv.close()


if __name__ == "__main__":
    main()
