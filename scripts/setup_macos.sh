#!/usr/bin/env bash
# AudioPipe — one-shot macOS build + install (the RECEIVE side).
#
# Does the whole Mac setup in one command by calling the component scripts:
#   1. build      — cmake configure + build (core, receiver, tools, tests)
#   2. device     — build + install the "AudioPipe" virtual input device
#                   (driver-mac, a BlackHole fork; the install needs sudo)
#   3. auto-start — install the receiver as a launchd job (starts at login,
#                   auto-restarts on crash)
#
# Re-run safe (idempotent). The Windows half is set up by scripts/setup_windows.ps1.
#
#   scripts/setup_macos.sh                 # build + device + auto-start
#   scripts/setup_macos.sh --tests         # also run the unit tests
#   scripts/setup_macos.sh --no-driver     # skip the device (e.g. already installed)
#   scripts/setup_macos.sh --no-autostart  # build (+device) only, no launchd
#   scripts/setup_macos.sh --daemon        # launchd as a LaunchDaemon (see receiver/README.md)
#   scripts/setup_macos.sh --uninstall     # remove the launchd job and the device
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

do_build=1; do_driver=1; do_autostart=1; run_tests=0; daemon=0; uninstall=0

usage() { awk 'NR>1 && /^#/{sub(/^# ?/,""); print; next} NR>1{exit}' "$0"; }

for a in "$@"; do
    case "$a" in
        --no-build)      do_build=0 ;;
        --no-driver)     do_driver=0 ;;
        --no-autostart)  do_autostart=0 ;;
        --tests)         run_tests=1 ;;
        --daemon)        daemon=1 ;;
        --uninstall)     uninstall=1 ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "unknown arg: $a (try --help)" >&2; exit 2 ;;
    esac
done

daemon_flag=""; [ "$daemon" = 1 ] && daemon_flag="--daemon"

if [ "$uninstall" = 1 ]; then
    echo ">>> Uninstalling receiver auto-start + AudioPipe device"
    ./receiver/uninstall_launchd.sh $daemon_flag || true
    if [ -d /Library/Audio/Plug-Ins/HAL/AudioPipe.driver ]; then
        echo "Removing the AudioPipe device (needs sudo)..."
        sudo rm -rf /Library/Audio/Plug-Ins/HAL/AudioPipe.driver
        sudo killall coreaudiod 2>/dev/null || true
    fi
    echo "Done."
    exit 0
fi

if [ "$do_build" = 1 ]; then
    echo ">>> [1/3] Building (cmake)"
    gen=""; command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
    cmake -S . -B build $gen
    cmake --build build
    if [ "$run_tests" = 1 ]; then
        ctest --test-dir build --output-on-failure
    fi
fi

if [ "$do_driver" = 1 ]; then
    echo ">>> [2/3] Building + installing the AudioPipe device (install needs sudo)"
    (cd driver-mac && ./build_driver.sh)
    sudo ./driver-mac/install.sh
fi

if [ "$do_autostart" = 1 ]; then
    echo ">>> [3/3] Installing the receiver auto-start (launchd)"
    ./receiver/install_launchd.sh $daemon_flag
fi

cat <<EOF

==== macOS setup done ====
The Mac is the RECEIVE side. Next:
  * Set the Windows sender's config dest_ip to this Mac's IP:
      ipconfig getifaddr en0   ->  $(ipconfig getifaddr en0 2>/dev/null || echo '<this Mac IP>')
  * Make sure UDP $(grep -oE '"listen_port"[^,]*' config/receiver.json | grep -oE '[0-9]+' || echo 4010) is allowed through the macOS firewall.
  * In any app, pick "AudioPipe" as the input/microphone to hear the stream.
Tune playout latency via "latency_ms" in config/receiver.json (see receiver/README.md).
Uninstall everything: scripts/setup_macos.sh --uninstall
EOF
