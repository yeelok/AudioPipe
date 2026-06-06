# AudioPipe

Low-latency, bit-transparent audio link: **Windows PC → macOS** over a local network
(UDP, raw PCM, 48 kHz / 32-bit float / stereo).

- Requirements: [spec.md](spec.md)
- Milestone plan & design decisions: [docs/plan.md](docs/plan.md)

## Architecture

A portable C++ **`core/`** library holds all the bug-prone logic (wire format, jitter buffer,
lock-free ring, drift control) and is unit-tested on either OS. The platform halves are thin
adapters: a Windows **sender** (WASAPI loopback capture → UDP) and a macOS **receiver** (UDP →
BlackHole-fork virtual input device).

## Repo layout

| Path | What | Milestone |
|---|---|---|
| `core/` | Portable C++17 library (packet, jitter, ring, drift, config, log) | M1–M2 |
| `tests/` | Catch2 unit tests for the core | M1+ |
| `tools/` | Loopback harness + latency probe | M1 |
| `receiver/` | macOS receiver process | M3 |
| `driver-mac/` | BlackHole fork (virtual input device) | M3 |
| `sender/` | Windows sender service | M4 |
| `driver-win/` | Virtual-Audio-Driver fork (virtual output device) | M4 |
| `config/` | Sample `sender.json` / `receiver.json` | — |

## Build (portable core + tests)

Requires **CMake ≥ 3.20**, a **C++17** compiler, **libsoxr** (`brew install libsoxr`), and
network access on first configure (CMake fetches nlohmann/json and Catch2).

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Loopback harness (M1)

Runs audio through the real wire path — frames → §7 packets → UDP over `127.0.0.1` → jitter
buffer → frames — with optional packet loss / reordering, then reports how faithfully the
output matches the input. Generates a sine signal by default, or reads a WAV with `--in`.
Writes `<prefix>-input.wav` and `<prefix>-output.wav`. (POSIX sockets; macOS/Linux only.)

```sh
build/tools/loopback_harness                          # clean path -> bit-transparent: YES
build/tools/loopback_harness --drop 0.02              # 2% loss -> concealment
build/tools/loopback_harness --reorder 0.10           # reordering -> corrected, bit-exact
build/tools/loopback_harness --in song.wav --out-prefix /tmp/t
```

Flags: `--frames 48..240` (samples/frame), `--channels`, `--rate`, `--seconds`, `--drop P`,
`--reorder P`, `--jitter DEPTH`, `--port`, `--seed`, `--in FILE.wav`, `--out-prefix`.

## Drift simulation (M2)

Models the receiver pipeline (variable-rate resampler + PI drift controller + ring buffer)
under a sender/consumer clock offset, with no wall clock — validates spec §8.4 in accelerated
form. Exits non-zero if the buffer under/overruns.

```sh
build/tools/drift_sim --ppm 500 --minutes 60          # corrected -> STABLE, centered
build/tools/drift_sim --ppm 200 --minutes 60 --no-correct   # -> GLITCHED (overflow)
build/tools/drift_sim --ppm 300 --trace               # print fill/ratio trajectory
```

## macOS device + receiver (M3)

The **AudioPipe** virtual input device is a BlackHole fork in `driver-mac/` (build-time
customized to AudioPipe / 2ch / 48 kHz). The **receiver** (`receiver/`) listens for UDP,
runs the core pipeline (jitter buffer → drift-corrected resampler → lock-free ring), and
plays into that device via a CoreAudio HAL output unit; apps select "AudioPipe" as a mic.

```sh
# 1. Build + install the device (sudo restarts coreaudiod):
(cd driver-mac && ./build_driver.sh && sudo ./install.sh)

# 2. End-to-end test on this Mac (sine, or pass a WAV); then select
#    "AudioPipe" as input in QuickTime / System Settings to hear it:
scripts/mac_e2e.sh
scripts/mac_e2e.sh song.wav
```

The receiver and `udp_send` (a real-time §7 sender that stands in for the Windows half) build
with the rest via CMake. See [driver-mac/README.md](driver-mac/README.md) for signing notes.

## Windows sender + device (M4)

The Windows half (build/test on the **Windows PC** — needs MSVC; not buildable on macOS):

- **`sender/`** — WASAPI shared-mode loopback capture → §7 packets → UDP, reusing `core/`.
  Runs as a Windows Service (§5.6) or `--console`. See [sender/README.md](sender/README.md).
- **`driver-win/`** — fork of [Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver)
  (Sysvad-based) as the virtual **output** device named "AudioPipe". Built with the WDK +
  test-signing. See [driver-win/README.md](driver-win/README.md).

The sender can capture the default output (or any virtual cable) for incremental testing
before the driver fork is finished — set `device_name` accordingly in `config/sender.json`.

## Status

**M4 — Windows sender + driver fork (code complete; build/test on Windows).** Wrote the WASAPI
loopback sender (capture → packetize → UDP), the Windows Service wrapper, and the
Virtual-Audio-Driver fork guide. `core` now finds libsoxr cross-platform (pkg-config or
vcpkg). The sender is **build-blind from this macOS session** — compile and test it on the
Windows PC. macOS side unaffected: 25 unit tests green. Next is **M5** — real end-to-end over
the LAN + latency tuning (needs both machines).
