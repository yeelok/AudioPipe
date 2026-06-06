#!/usr/bin/env bash
# Install the AudioPipe HAL plug-in. Copies the bundle into the system HAL
# directory and restarts coreaudiod (audio drops out briefly). Reversible:
#   sudo rm -rf /Library/Audio/Plug-Ins/HAL/AudioPipe.driver && sudo killall coreaudiod
set -euo pipefail
cd "$(dirname "$0")"

DRIVER="build/AudioPipe.driver"
DEST="/Library/Audio/Plug-Ins/HAL"

if [ ! -d "$DRIVER" ]; then
    echo "Driver not built. Run ./build_driver.sh first." >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root: sudo ./install.sh" >&2
    exit 1
fi

rm -rf "$DEST/AudioPipe.driver"
cp -R "$DRIVER" "$DEST/"
killall coreaudiod 2>/dev/null || true
echo "Installed AudioPipe.driver. Open 'Audio MIDI Setup' or run:"
echo "  system_profiler SPAudioDataType | grep AudioPipe"
