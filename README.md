# AudioPipe

Low-latency, bit-transparent audio link: **Windows PC → macOS** over a local network
(UDP, raw PCM, 48 kHz / 32-bit float / stereo).

- Requirements: [spec.md](spec.md)
- Milestone plan & design decisions: [docs/plan.md](docs/plan.md)

## Architecture

The low-latency send/receive engine is the [**AOO**](https://git.iem.at/aoo/aoo)
(Audio over OSC) library — it owns the UDP wire protocol, jitter buffer, time-DLL
clock-drift correction, **dynamic resampling** (so source and sink can run at
different/​drifting rates), and packet resend. The platform halves are thin
adapters around AOO's bare `AooSource`/`AooSink` (we provide the sockets, the
NET/discovery layer is off): a Windows **sender** (WASAPI loopback capture →
`AooSource` → UDP) and a macOS **receiver** (UDP → `AooSink` → BlackHole-fork
virtual input device). A small **`core/`** library holds config, logging, and the
fixed 48 kHz/float32 format constants.

> AOO is PCM (bit-transparent, Opus off) and float32, fetched at configure time
> via CMake `FetchContent` (Standard Improved BSD license). See
> [docs/plan.md](docs/plan.md) for why AOO replaced the original hand-rolled
> packet/jitter/drift modules.

## Repo layout

| Path | What |
|---|---|
| `core/` | Config, logging, format constants (C++17) |
| `tests/` | Catch2 unit tests (incl. an in-process AOO loopback) |
| `tools/` | `aoo_send` — real-time WAV→AooSource sender (Mac test stand-in) |
| `receiver/` | macOS receiver process (AooSink → CoreAudio) |
| `driver-mac/` | BlackHole fork (virtual input device) |
| `sender/` | Windows sender service (WASAPI → AooSource) |
| `driver-win/` | Virtual-Audio-Driver fork (virtual output device, 48 kHz) |
| `config/` | Sample `sender.json` / `receiver.json` |

## Build (core + tests)

Requires **CMake ≥ 3.20**, a **C++17** compiler, and network access on first
configure (CMake fetches nlohmann/json, Catch2, and AOO). No system audio libs
are needed — AOO is built from source into the binaries.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## macOS device + receiver

The **AudioPipe** virtual input device is a BlackHole fork in `driver-mac/` (build-time
customized to AudioPipe / 2ch / 48 kHz). The **receiver** (`receiver/`) listens for UDP,
runs the AOO sink (jitter buffer → drift-corrected dynamic resampling), and plays into that
device via a CoreAudio HAL output unit; apps select "AudioPipe" as a mic.

```sh
# 1. Build + install the device (sudo restarts coreaudiod):
(cd driver-mac && ./build_driver.sh && sudo ./install.sh)

# 2. End-to-end test on this Mac (sine, or pass a WAV); then select
#    "AudioPipe" as input in QuickTime / System Settings to hear it:
scripts/mac_e2e.sh
scripts/mac_e2e.sh song.wav
```

`scripts/mac_e2e.sh` runs the receiver and `aoo_send` (a real-time WAV→`AooSource` sender that
stands in for the Windows half) on one machine. `aoo_send` streams at the WAV's own rate, so it
also exercises AOO's cross-rate resampling (e.g. a 44.1 kHz file → the 48 kHz device). See
[driver-mac/README.md](driver-mac/README.md) for signing notes and [receiver/README.md](receiver/README.md)
for the `latency_ms` tuning knob.

## Windows sender + device (M4)

The Windows half (build/test on the **Windows PC** — needs MSVC; not buildable on macOS).
**First time? Follow [docs/windows-bringup.md](docs/windows-bringup.md)** — a step-by-step
checklist for compiling and validating the AOO sender + driver (none of it has been built on
Windows since the migration).

- **`sender/`** — WASAPI shared-mode loopback capture → `AooSource` → UDP. AOO streams at the
  captured rate and the Mac sink resamples to 48 kHz, so a non-48 kHz endpoint no longer
  glitches. Runs as a Windows Service (§5.6) or `--console`. See [sender/README.md](sender/README.md).
- **`driver-win/`** — fork of [Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver)
  (Sysvad-based) as the virtual **output** device named "AudioPipe". Built with the WDK +
  test-signing. See [driver-win/README.md](driver-win/README.md).

The sender can capture the default output (or any virtual cable) for incremental testing
before the driver fork is finished — set `device_name` accordingly in `config/sender.json`.

## Status

**Transport migrated to AOO.** The original hand-rolled packet/jitter/drift/resampler core was
replaced by the AOO library (`AooSource`/`AooSink`). This fixed the sample-rate-mismatch dropout
bug (a non-48 kHz source is now resampled instead of starving the device) and added time-DLL
drift correction + packet resend. On-Mac e2e is glitch-free (0 over/underruns) at the default
`latency_ms=50` for both 48 kHz and 44.1 kHz sources; all 11 unit tests pass.

The ≤20 ms target (§9) is the open follow-up. The on-Mac loopback needs ~50 ms because of
**arrival-timing jitter on a single contended box** — the receiver's fill-ratio log shows the
buffer swinging 0.04..1.00 at low latency while the **measured device rate stays dead-on 48000 Hz
(no drift)**; at 50 ms it settles to a tight 0.42..0.44. It's not the sender's pacing (making the
sender burst-free didn't help — the jitter is receiver/scheduling-side). On the real link the two
halves run on separate machines with a hardware-clocked WASAPI capture, so low-latency tuning
belongs there. See [docs/plan.md](docs/plan.md).

**M5–M7 — running end-to-end over the LAN, with operational logging and auto-start.**

- **M4 built & tested on Windows** (VS 2022/MSVC) — *for the original pre-AOO sender*: the
  WASAPI-loopback → UDP path was verified on the wire and §5.7 resilience held. The AOO-based
  sender rewrite has not yet been re-built on Windows (see the AOO note above).
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
