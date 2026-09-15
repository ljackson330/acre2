#!/usr/bin/env bash
# Phase 0 capture harness (Linux/PipeWire).
#
# Records the audio output of a single application node -- the Linux equivalent
# of Windows AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK. Targeting is by
# PipeWire object.serial, which maps 1:1 to application.process.id, so this
# satisfies the gameplan's hard requirement: only the named process is captured,
# never the device mix.
#
# NOTE ON WINE VERSIONS: Arma runs under Proton's bundled Wine; the TS3 client
# (and therefore the ACRE2 plugin) runs under system Wine. That split is
# expected and does not affect this harness -- PipeWire sees both as ordinary
# application nodes regardless of which Wine produced them.
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: phase0_capture.sh list
       phase0_capture.sh record <seconds> [outfile.wav] [--target SERIAL]

  list     show every application currently producing audio, with PID
  record   capture one application's output to a WAV

Auto-detects Arma when exactly one candidate is playing; otherwise pass
--target with a serial from `list`. Analyse the result with wav_stats.py.
USAGE
    exit 1
}

nodes() {
    # pactl prints Corked: before the properties block, so each record is
    # buffered and flushed when the next one starts (or at EOF).
    pactl list sink-inputs | awk '
        function flush() {
            if (serial != "")
                printf "%s\t%s\t%s\t%s\t%s\n",
                       (oserial != "" ? oserial : serial), pid, bin, corked, name
            serial=""; oserial=""; pid="?"; bin="?"; corked="?"; name="?"
        }
        function val(line,   v) { v=line; sub(/.*= "/,"",v); sub(/"$/,"",v); return v }
        /^Sink Input #/                  { flush(); serial=substr($3,2) }
        /^\tCorked:/                     { corked=$2 }
        /object\.serial =/               { oserial=val($0) }
        /application\.name =/            { name=val($0) }
        /application\.process\.binary =/ { bin=val($0) }
        /application\.process\.id =/     { pid=val($0) }
        END { flush() }
    '
}

cmd_list() {
    printf '%-8s %-8s %-18s %-8s %s\n' SERIAL PID BINARY CORKED NAME
    nodes | while IFS=$'\t' read -r serial pid bin corked name; do
        printf '%-8s %-8s %-18s %-8s %s\n' "$serial" "$pid" "$bin" "$corked" "$name"
    done
    echo
    echo "Corked=yes means the stream is paused and will capture as 0 frames."
}

find_arma() {
    nodes | grep -iE 'arma|steam_app_107410' | cut -f1 || true
}

cmd_record() {
    local secs="$1"; shift
    local out="${1:-}"; [[ "${out:-}" == --* ]] && out=""
    [[ -n "$out" ]] && shift || out="capture_$(date +%H%M%S).wav"
    local target=""
    [[ "${1:-}" == "--target" ]] && target="$2"

    if [[ -z "$target" ]]; then
        mapfile -t found < <(find_arma)
        if [[ ${#found[@]} -eq 1 ]]; then
            target="${found[0]}"
            echo "auto-detected Arma at serial $target"
        elif [[ ${#found[@]} -eq 0 ]]; then
            echo "No Arma audio node found. Is Arma running AND producing sound?" >&2
            echo "Run 'phase0_capture.sh list' and pass --target explicitly." >&2
            exit 1
        else
            echo "Multiple Arma nodes (${found[*]}) -- pass --target explicitly." >&2
            exit 1
        fi
    fi

    echo "recording ${secs}s from serial $target -> $out"
    timeout "$secs" pw-record --target "$target" "$out" || true
    echo
    python3 "$(dirname "$0")/wav_stats.py" "$out"
}

case "${1:-}" in
    list)   cmd_list ;;
    record) shift; [[ $# -ge 1 ]] || usage; cmd_record "$@" ;;
    *)      usage ;;
esac
