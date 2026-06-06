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

## Status

**M0 — project scaffold.** Core library and test target build; JSON config and the §7 packet
header layout are implemented and tested. Per-milestone implementations land behind the
`TODO(Mx)` markers in `core/include/audiopipe/`.
