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

## Pipeline check — core fidelity on real audio (Windows + macOS)

`pipeline_check` runs a real WAV through the **complete receiver DSP path** the
live system uses — jitter buffer → drift-corrected resampler → lock-free ring →
device-rate consumer — with **no network and no audio hardware**, and writes an
output WAV you can listen to plus a stability/fidelity report. It's the offline
way to answer "is the distortion our pipeline, or the environment?": if the
output is clean here, the core is fine and any live glitching is the
network/device side. Unlike the harnesses above it's **portable (no sockets)**,
so it builds and runs identically on **both** machines (it's part of the normal
`cmake --build build`; on Windows put vcpkg's `soxr.dll` on `PATH` to run it,
same as the other binaries).

```sh
# Run your own audio through the full pipeline and listen to the result:
build/tools/pipeline_check --in song.wav --out out.wav         # clean -> STABLE
build/tools/pipeline_check --in song.wav --ppm 500             # +500 ppm clock drift, corrected
build/tools/pipeline_check --in song.wav --drop 0.02 --reorder 0.05   # loss/reorder -> concealment
build/tools/pipeline_check --ppm 3000 --no-correct             # generated sine -> GLITCHED (proves detection)
```
```bat
:: Windows (PowerShell), with soxr.dll reachable:
build\tools\pipeline_check.exe --in song.wav --out out.wav
```

Flags: `--in FILE` / `--out FILE`, `--frames` (jitter frame size), `--jitter-frames`
(buffer depth), `--ppm` (sender/receiver clock offset), `--drop P`, `--reorder P`,
`--no-correct` (disable drift correction), `--seconds`/`--rate`/`--channels` (for the
generated sine when no `--in`), `--seed`. A fault-free run prints `RESULT: STABLE`
and exits 0; under/overruns or NaNs print `GLITCHED` and exit non-zero (handy in CI).

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

**M5–M7 — running end-to-end over the LAN, with operational logging and auto-start.**

- **M4 built & tested on Windows** (VS 2022/MSVC, vcpkg `soxr`): one include-order fix and the
  sender compiles; `core` 25/25 tests pass; the WASAPI-loopback → §7 → UDP path is verified on
  the wire, and §5.7 resilience (no crash / recovers when the receiver is down) holds.
- **M5 — real Windows → macOS link works**, loss-free, audible on the Mac. Latency: on **Wi-Fi**
  the network alone is ~18 ms one-way with ~17 ms jitter, so the ≤20 ms target (a **wired**-LAN
  spec, §9) is not met over Wi-Fi — the pipeline itself sits at ~9 ms on a clean link. **Use
  wired Ethernet for the ≤20 ms acceptance** (retest pending).
- **M6 — config + logging (§10) complete:** JSON both sides, plus a sender heartbeat and the
  receiver's loss/drift/ring logging; clean signal shutdown.
- **M7 — auto-start (code complete; install + reboot are user steps):** Windows sender as a
  crash-restarting auto-start Service (`scripts/install_sender_service.ps1`), macOS receiver as a
  LaunchAgent/LaunchDaemon (`receiver/install_launchd.sh`).

Remaining: the **wired ≤20 ms** retest and the **2-hour soak** (M8), and the optional
`driver-win` virtual-output fork (WDK build) so Windows apps route per-app instead of capturing
the default device.
