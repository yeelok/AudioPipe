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

Requires **CMake ≥ 3.20**, a **C++17** compiler, and network access on first configure
(CMake fetches nlohmann/json and Catch2).

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

## Status

**M1 — portable core + loopback.** Packet codec, lock-free SPSC ring buffer, and fixed-depth
jitter buffer (reorder / repeat-last concealment / late-drop / priming) are implemented and
unit-tested (22 cases). The loopback harness confirms a clean run is bit-transparent, injected
loss is concealed 1:1, and reordering is corrected. Drift correction (M2) is next: see the
`TODO(M2)` markers in `core/include/audiopipe/{drift_control,resampler}.h`.
