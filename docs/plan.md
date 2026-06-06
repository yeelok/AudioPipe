# Implementation Plan: Low-Latency Audio Link (Windows PC → macOS)

> Companion to [spec.md](../spec.md). This is the *how*; the spec is the *what*.

## Guiding principle

Every bug-prone algorithm lives in a **portable C++ `/core`** library and is testable on
the Mac with no drivers and no audio hardware. The platform-specific halves (Windows sender,
macOS receiver) become thin adapters around already-tested code. This is what lets us retire
~60–70% of the project's risk before ever touching a kernel driver or a second machine.

## Architecture overview

```
        WINDOWS                         PORTABLE C++ CORE                    macOS
┌──────────────────────┐        ┌──────────────────────────┐      ┌──────────────────────┐
│ driver-win (fork)    │        │ packet (wire format §7)  │      │ driver-mac (BlackHole │
│  virtual OUTPUT dev   │        │ jitter_buffer (§8.1-8.3) │      │   fork) virtual INPUT │
│ sender:               │        │ ring_buffer (lock-free)  │      │ receiver:             │
│  WASAPI loopback cap → │──UDP──▶│ drift_control (§8.4)     │─────▶│  UDP recv → core →    │
│  core → sendto()      │        │ config / log             │      │  write into driver    │
└──────────────────────┘        └──────────────────────────┘      └──────────────────────┘
        shared, unit-tested on either machine
```

## Repo layout

```
AudioPipe/
  core/
    packet.{h,cpp}        # encode/decode §7 header, magic/version, seq, format id
    jitter_buffer.{h,cpp} # fixed depth (default 3), reorder + loss detection
    ring_buffer.{h,cpp}   # lock-free SPSC, audio-callback safe
    drift_control.{h,cpp} # fill-level PI controller → soxr variable ratio
    resampler.{h,cpp}     # libsoxr wrapper (variable-rate mode)
    config.{h,cpp}        # JSON config loader
    log.{h,cpp}           # file logger, no alloc on the audio path
    format.h              # constants: 48k, f32, stereo, frame sizes
  sender/      (Windows)  # WASAPI loopback + UDP send + Windows Service wrapper
  receiver/    (macOS)    # UDP recv + write to driver-mac + LaunchDaemon plist
  driver-mac/  (fork of BlackHole)
  driver-win/  (fork of Scream or VirtualAudioDriver — see decision log)
  tools/
    loopback_harness/     # fake sender + fake receiver, WAV in/out, fault injection
    latency_probe/        # measures end-to-end latency
  tests/                  # unit tests for /core
  config/                 # sample sender.json, receiver.json
  docs/                   # this plan + per-platform README + build/run/auto-start
```

## Milestones

### M0 — Project skeleton & build system  ✅ DONE
- CMake (≥3.20) top-level + `core/` + `tests/`. Deps via FetchContent: nlohmann/json 3.11.3,
  Catch2 v3.7.1. libsoxr deferred to M2 (only needed then).
- `audiopipe_core` static lib builds clean (`-Wall -Wextra -Wpedantic`); `core_tests` runs
  green via CTest (7/7). Verified on macOS (Apple clang 21, Ninja).
- Real content shipped (not just stubs): `format.h` constants + §7 `PacketHeader` (18-byte
  packed, layout-asserted), working JSON config loader with defaults/validation, minimal file
  logger. Remaining modules are documented header interfaces behind `TODO(Mx)` markers, kept
  honest by `compile_check.cpp`.
- **Deliverable met:** libs build + tests pass. (Windows build to be confirmed at M4.)

### M1 — Portable core + loopback harness (all on Mac)  ✅ DONE
Most risk dies here, before any driver work.
- `packet` (§7) — encode/decode, explicit little-endian wire, seq-wrap, truncation/bad-magic
  rejection, overflow. ✅
- `jitter_buffer` — fixed depth, reorder correction, repeat-last concealment (§8.2),
  late-drop (§8.3), priming (§8.1), loss/late/overflow counters. ✅
- `ring_buffer` — lock-free SPSC, acquire/release, validated with a 200k-item threaded
  stress test. ✅
- `loopback_harness` (`tools/`) — sine or WAV → real packetizer → UDP `127.0.0.1` → jitter
  buffer with lock-step playout → WAV, plus deterministic **fault injection** (drop %, reorder)
  and a fidelity report. Minimal WAV read/write (`tools/wav.*`). ✅
- **Acceptance met:** clean run bit-identical (`mismatched=0`); 2% loss → exactly 28/28 frames
  concealed 1:1; 10% reorder → fully corrected, bit-exact. 22 unit tests green.
- *Harness note:* playout pops one frame per arrival once primed, keeping buffer fill ≈ depth
  (an insert-all-then-drain model overflowed the finite reorder window — fixed).

### M2 — Drift correction in core (§8.4 — hardest algorithm)  ✅ DONE
- `resampler` — libsoxr variable-rate (`SOXR_HQ | SOXR_VR`), interleaved float32, live ratio
  via `soxr_set_io_ratio`. ✅
- `drift_control` — PI loop on capacity-normalized fill error, output/input ratio clamped to
  ±0.1% (§8.4), anti-windup on the integral. Defaults tuned ~critically-damped. ✅
- `tools/drift_sim` + 3 unit tests — deterministic producer/consumer ppm model through the
  real resampler + ring. ✅
- **Acceptance met:** 60-min run at ±500 ppm holds fill *exactly* on target (drift 0), zero
  under/overruns, steady-state ratio = 1∓ppm (smooth, no steps); uncorrected control overflows
  (29k glitches). libsoxr 0.1.3 via Homebrew/pkg-config. 25 unit tests green.

### M3 — macOS receiver + BlackHole fork (Mac only)  ✅ code complete (install = user step)
- Forked BlackHole → `driver-mac/` as the **AudioPipe** device; customized via build-time
  constants (`build_driver.sh`): name AudioPipe, bundle `audio.audiopipe.AudioPipe`, 2ch,
  48 kHz, ad-hoc signed. Builds to `AudioPipe.driver` (universal). `install.sh` copies to
  `/Library/Audio/Plug-Ins/HAL/` + restarts coreaudiod. ✅
- Separate **receiver process** (`receiver/`, §6.3): UDP recv → JitterBuffer → drift-corrected
  Resampler → SpscRingBuffer → CoreAudio HAL output unit render callback. Device found by name;
  small IO buffer requested for low latency. ✅
- `tools/udp_send` — real-time §7 sender (stand-in for the Windows half); `scripts/mac_e2e.sh`
  orchestrates the local test. ✅
- **Acceptance (pending user step):** requires `sudo ./install.sh` then `scripts/mac_e2e.sh`
  to confirm audio is audible/selectable in QuickTime. Code builds clean; 25 unit tests green.
  Ad-hoc-signed HAL plug-in loading on Apple Silicon may need a Developer ID — see
  driver-mac/README.md.

### M4 — Windows sender + driver fork (Windows)
- Fork the chosen driver, build with WDK, enable test-signing, load, confirm it's a
  per-app-routable output.
- Sender: WASAPI **loopback capture** in shared mode on the virtual device → core
  packetizer → `sendto`. Develop capture against a normal device first, then point at the fork.
- **Acceptance:** a single app routed to the virtual device; its audio captured and sent.

### M5 — Real end-to-end over LAN
- Connect M3 + M4. Reconnection / no-crash on brief network loss (§5.7).
- Measure latency with `latency_probe`; tune frame size + jitter depth to hit ≤20 ms.

### M6 — Config + logging (§10)
- JSON config both sides; log startup/shutdown, device state, loss counts, drift ratio,
  under/overruns. Clean shutdown on signal.

### M7 — Auto-start (§5.6, §6.4)
- Windows Service (or Scheduled Task) + macOS LaunchDaemon plist.
- Verify both start after reboot with no login.

### M8 — Soak + acceptance
- Full 2-hour session: no glitches, no cumulative drift. Re-measure latency. Validate all §13.
- **(Optional M9):** merge receiver into the BlackHole plugin per §6.3 / order #8.

## Decisions to lock before M0/M1

| Decision | Recommendation | Status |
|---|---|---|
| Windows driver fork | See decision log below | researching |
| Config format | **JSON via nlohmann/json** (header-only) | **decided** |
| Frames per packet (§7) | Start at 1 (lowest latency), make configurable | proposed |
| Drift controller placement | Resample on receiver before the ring; keep sender dumb | proposed |
| Test framework | **Catch2 v3** (built-in benchmarking + float matchers) | **decided** |

## Why this order works for two machines

M1–M2 (the algorithmically risky majority) and M3 happen entirely on the Mac via the loopback
harness. The Windows box isn't needed until M4, by which point the wire format, jitter buffer,
and drift control are proven — so the Windows side is just "capture + sendto" against tested
code. True cross-machine work only begins at M5, when both halves are individually validated.

## Decision log

### Windows driver: Scream vs Virtual-Audio-Driver — **DECISION: Virtual-Audio-Driver**

**Chosen:** Fork [`VirtualDrivers/Virtual-Audio-Driver`](https://github.com/VirtualDrivers/Virtual-Audio-Driver)
as a clean virtual **output** endpoint, and capture it from a **userspace WASAPI loopback
sender** (all transport logic stays in `/core`).

**Rejected:** Scream — its networking runs *in the kernel* with no userspace portion. That
fights this spec's architecture head-on (§3 / §5.3 put the sender in userspace C++) and would
force custom-header UDP + drift resampling into kernel mode: undebuggable, can't reuse `/core`,
can't host libsoxr. Scream's model is "the sound card itself broadcasts," which is the wrong
abstraction here. It's also based on the older MSVAD sample and documents no per-app routing.

| Factor | Virtual-Audio-Driver | Scream |
|---|---|---|
| Sample base | **Sysvad** (modern, MS-recommended) | MSVAD (older) |
| Networking | None — we do it in userspace ✅ | In kernel ❌ (conflicts with spec) |
| Per-app routing | Yes, via Volume Mixer (matches §5.1) ✅ | Not documented |
| Devices created | Virtual speaker **+** mic (we use speaker) | Virtual speaker |
| License | MIT (+ MS-PL samples) ✅ | MS-PL |
| Maintenance | Active, signed releases 2025 ✅ | Mature but model-mismatched |
| OS / arch | Win10 1903+ / Win11, x64 + ARM64 | Win10/11 |
| Fork effort | Low — mainly rename; capture is separate userspace work | High — kernel UDP rewrite |

**Implication for M4:** the driver fork is *minimal* (rename device/IDs, build, test-sign,
confirm it's a routable shared-mode output). The real sender work — WASAPI loopback capture →
`/core` packetizer → `sendto` — is ordinary userspace C++ developed against a normal device
first, then pointed at the fork. This keeps the Windows-specific surface small.
