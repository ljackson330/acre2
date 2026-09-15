#!/usr/bin/env bash
# Records a demo of ambient battle sound through the real TeamSpeak pipeline.
#
# Captures TeamSpeak's own output node, not Arma's. With TeamSpeak's local
# microphone playback enabled, that node carries exactly what the capture
# pipeline produced -- so the recording is evidence the game audio genuinely
# went through the plugin's mix into the outgoing stream, not merely that Arma
# was making noise nearby.
#
# Setup in TeamSpeak first:
#   Tools -> Options -> Capture -> Begin Test  (leaves local playback on)
# Then talk while transmitting on an ACRE2 radio near some combat.
set -euo pipefail

SECONDS_TO_RECORD="${1:-30}"
OUT="${2:-demo_$(date +%H%M%S).wav}"
HERE="$(cd "$(dirname "$0")" && pwd)"

serial=$(pactl list sink-inputs | awk '
    /^Sink Input #/          { s=substr($3,2) }
    /object\.serial =/       { split($0,a,"\""); s=a[2] }
    /application\.name =/    { split($0,a,"\""); if (tolower(a[2]) ~ /teamspeak/) { print s; exit } }
')

if [[ -z "${serial:-}" ]]; then
    echo "No TeamSpeak audio node found." >&2
    echo "TeamSpeak must be running AND producing sound -- enable local mic" >&2
    echo "playback via Tools -> Options -> Capture -> Begin Test." >&2
    exit 1
fi

echo "recording TeamSpeak output (node $serial) for ${SECONDS_TO_RECORD}s -> $OUT"
echo "talk on the radio now, near some combat"
timeout "$SECONDS_TO_RECORD" pw-record --target "$serial" "$OUT" || true

echo
python3 "$HERE/wav_stats.py" "$OUT" 2
