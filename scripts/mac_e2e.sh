#!/usr/bin/env bash
# On-Mac end-to-end test (M3 acceptance): starts the receiver feeding the
# AudioPipe device, then streams audio to it with the real-time sender. Select
# "AudioPipe" as the input in QuickTime / System Settings to hear it.
#
#   scripts/mac_e2e.sh                 # 440/660 Hz sine, looping
#   scripts/mac_e2e.sh path/to.wav     # stream a WAV file, looping
#
# Requires the driver to be built and installed first:
#   (cd driver-mac && ./build_driver.sh && sudo ./install.sh)
set -euo pipefail
cd "$(dirname "$0")/.."

RECV=build/receiver/audiopipe_receiver
SEND=build/tools/udp_send
CFG=config/receiver.json
PORT="${AUDIOPIPE_PORT:-4010}"
IN="${1:-}"

if ! system_profiler SPAudioDataType 2>/dev/null | grep -qi audiopipe; then
    echo "AudioPipe device not found. Build & install the driver first:" >&2
    echo "  (cd driver-mac && ./build_driver.sh && sudo ./install.sh)" >&2
    exit 1
fi
if [ ! -x "$RECV" ] || [ ! -x "$SEND" ]; then
    echo "Binaries missing. Build first:  cmake --build build" >&2
    exit 1
fi

echo "Starting receiver (port $PORT -> AudioPipe)..."
"$RECV" --config "$CFG" &
RECV_PID=$!
trap 'kill "$RECV_PID" 2>/dev/null || true' EXIT
sleep 1

cat <<'EOF'

>>> Now select "AudioPipe" as an input to hear the stream:
    - QuickTime Player > File > New Audio Recording > choose "AudioPipe", or
    - System Settings > Sound > Input > "AudioPipe" (watch the level meter).

Streaming (looping). Press Ctrl-C to stop.
EOF

if [ -n "$IN" ]; then
    "$SEND" --in "$IN" --port "$PORT" --loop
else
    "$SEND" --seconds 5 --port "$PORT" --loop
fi
