# AudioPipe

Low-latency, bit-transparent audio link: **Windows PC → macOS** over your LAN
(UDP · raw PCM · 48 kHz / 32-bit float / stereo). Route an app's audio on Windows
into the **AudioPipe** output device; hear it on the Mac as an **AudioPipe** input
("microphone") that any app — Zoom, OBS, QuickTime — can select.

**Status:** the macOS receive side is built and validated (end-to-end clean, 11/11
unit tests, a 20-minute soak with zero drops). The Windows sender builds and runs
locally; the **real two-machine link**, the **≤20 ms wired-latency** check, and the
**2-hour soak** are the open acceptance items — see [Status & roadmap](#status--roadmap).

## How it works

The send/receive engine is the [**AOO**](https://git.iem.at/aoo/aoo) (Audio over
OSC) library — it owns the UDP wire protocol, jitter buffer, time-DLL clock-drift
correction, **dynamic resampling** (source and sink may run at different / drifting
rates), and packet resend. The platform halves are thin adapters around AOO's bare
`AooSource` / `AooSink` (we own the sockets; AOO's discovery layer is off):

```
Windows app ─▶ AudioPipe output device (driver-win) ─▶ sender: WASAPI loopback ─▶ AooSource
     └─ UDP / LAN ─▶ receiver: AooSink ─▶ AudioPipe input device (driver-mac) ─▶ Mac app
```

PCM float32 with Opus off → **bit-transparent**. AOO is fetched at configure time
via CMake `FetchContent` (Standard Improved BSD). A small `core/` holds config,
logging, and the 48 kHz / float32 constants.

## Quick start — build & install (one command per machine)

Run each on its own machine; each script builds, then installs, by calling the
component scripts under the hood.

**macOS — the receive side:**

```sh
scripts/setup_macos.sh
```

Builds the project, builds + installs the **AudioPipe** virtual input device
(asks for `sudo`), and installs the receiver as a launchd auto-start job.
Flags: `--tests`, `--no-driver`, `--no-autostart`, `--daemon`, `--uninstall`.

**Windows — the send side (elevated PowerShell):**

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force   # if .ps1 is blocked — this window only, reverts on close
.\scripts\setup_windows.ps1 -Start
```

Builds the sender, builds the **AudioPipe** virtual output driver (needs the WDK),
and installs the persistent device + auto-start sender Service. The test-signed
driver only loads in **Test Mode**, which needs one reboot — so the **first** run
turns on test-signing and asks you to **reboot, then re-run**
`.\scripts\setup_windows.ps1 -SkipBuild -Start`.
Flags: `-SkipBuild`, `-SkipDriver`, `-Start`, `-Uninstall`.

Then set the Windows `config\sender.json` `dest_ip` to the Mac's IP, pick
**AudioPipe** as the input on the Mac, and route an app to the **AudioPipe**
output on Windows. (Make sure the receiver's UDP port — default 4010 — is open on
the Mac firewall.)

## Build & test (dev, no install)

Requires **CMake ≥ 3.20** and a **C++17** compiler; the first configure fetches
nlohmann/json, Catch2 and AOO (needs network + `git`). No system audio libs.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

On-Mac end-to-end check (no Windows box needed — streams a WAV/sine into the
installed device; pick **AudioPipe** as input to hear it):

```sh
scripts/mac_e2e.sh [song.wav]
```

## Components

| Path | What |
|---|---|
| `core/` | Config, logging, 48 kHz / float32 constants (C++17) |
| `tests/` | Catch2 unit tests, incl. an in-process `AooSource → AooSink` loopback |
| `tools/` | `aoo_send` (real-time WAV → `AooSource` UDP sender, Mac stand-in for the Windows half); `pipeline_check` (offline full-pipeline WAV → WAV with loss/reorder/drift injection) |
| `receiver/` | macOS receiver: UDP → `AooSink` → CoreAudio — [readme](receiver/README.md) (incl. the `latency_ms` knob) |
| `driver-mac/` | **AudioPipe** virtual input device — BlackHole fork, 2ch/48 kHz — [readme](driver-mac/README.md) |
| `sender/` | Windows sender: WASAPI loopback → `AooSource` → UDP; runs as a Service — [readme](sender/README.md) |
| `driver-win/` | **AudioPipe** virtual output device — Virtual-Audio-Driver fork, pinned to 48 kHz; WDK + test-signing — [readme](driver-win/README.md) |
| `scripts/` | `setup_macos.sh` / `setup_windows.ps1` (one-shot) + the install/uninstall component scripts |
| `config/` | Sample `sender.json` / `receiver.json` |

First time building on Windows? See the build + troubleshooting section of [docs/DESIGN.md](docs/DESIGN.md).
For incremental testing before the driver is built, the sender can capture your
**default output** — set `device_name: ""` in `config/sender.json`.

## Status & roadmap

**Done & validated**
- AOO transport on both halves; the macOS receiver is end-to-end clean at the
  default `latency_ms=50` for **48 kHz and 44.1 kHz** sources — AOO resamples, so
  the old sample-rate-mismatch dropout is gone.
- 11/11 unit tests (macOS and Windows/MSVC); a 20-minute on-Mac soak with **0
  over/underruns** and a rock-steady measured device rate.
- The Windows sender builds and streams **overflow-free** to a local listener.

**Open acceptance items**
- **Real two-machine Windows → Mac link** on the AOO build (so far: on-Mac and
  local-listener runs only).
- **≤20 ms on wired Ethernet (§9):** the on-Mac loopback needs ~50 ms purely from
  single-box scheduling jitter (the measured device rate is dead-on 48 kHz — not
  clock drift). This belongs on the real wired link.
- **2-hour soak** end to end (§8).
- Per-app routing uses the `driver-win` device — install it to route a single app
  instead of capturing the default output.

Requirements, architecture, design decisions (the AOO migration, the block-xrun
fix, the latency analysis), and acceptance criteria all live in one place:
[docs/DESIGN.md](docs/DESIGN.md).
