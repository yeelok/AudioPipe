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

### M4 — Windows sender + driver fork (Windows)  ◐ code complete (build/test on Windows)
- **Sender** (`sender/`, build-blind from macOS): `WasapiLoopbackCapture` (shared-mode
  loopback, float32), `UdpSender` (Winsock, best-effort per §5.7), `run_sender` loop
  (capture → §7 packetize → send, device/network reconnect), `main` (console + SCM dispatch),
  `service.cpp` (Windows Service, §5.6). Reuses `core` packet/config/log. ✅ written
- **Driver** (`driver-win/`): documented fork of Virtual-Audio-Driver (clone upstream, rename
  endpoint to "AudioPipe", WDK build, test-signing, pnputil install). Not vendored (WDK build
  is Windows-only). ✅ documented
- `core` libsoxr discovery made cross-platform (pkg-config → find_library/vcpkg fallback).
- **Acceptance (pending Windows build):** route one app to the AudioPipe output, run the
  sender, confirm capture+send. Sender can target the default device / a virtual cable first.
  Code compiles only under MSVC — verify on the Windows PC. macOS unaffected (25 tests green).

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

---

## Addendum: migration to the AOO library (post-M7)

The original `/core` carried hand-rolled transport: a §7 packet codec, jitter
buffer, PI drift controller, and libsoxr resampler. Real-world testing exposed
the predictable weakness — that pipeline assumed **every** stage ran at exactly
48 kHz and only had a ±0.1 % drift trim, so a non-48 kHz source (e.g. a Windows
endpoint left at 44.1 kHz, very common) starved the playout ring and produced
continuous dropouts ("splitting and noise").

Rather than build a full sample-rate converter + clock-sync layer ourselves, we
adopted **[AOO](https://git.iem.at/aoo/aoo)** (Audio over OSC) — the engine
behind SonoBus — via its bare `AooSource`/`AooSink` API (we keep our own UDP
sockets; AOO's NET/discovery layer is off). AOO provides, in tested form, exactly
the algorithmically risky pieces we were maintaining:

- UDP wire protocol + jitter buffer (auto-sizing, reorder/loss handling),
- **time-DLL clock-drift correction** ("Using a DLL to filter time", Adriaensen)
  with **dynamic resampling** — source and sink may run at different / drifting
  rates and AOO resamples between them,
- selective **packet resend** (NACK) for low-RTT LANs.

It's float32 + PCM (Opus disabled), so the path stays bit-transparent, and it's
Standard Improved BSD (license-compatible). Pulled via CMake `FetchContent`,
pinned to `v2.0-pre4`, matching how we already fetch nlohmann/json and Catch2.

**Removed:** `core/{packet,jitter_buffer,drift_control,resampler}.{h,cpp}`, the
libsoxr dependency, and the `loopback_harness`/`drift_sim`/`udp_send` tools (their
roles are now AOO's; the in-process `tests/test_aoo_loopback.cpp` + the on-device
`aoo_send` e2e cover them). **Kept:** config, logging, format constants.

**Result:** on-Mac e2e is glitch-free (0 over/underruns) at `latency_ms=50` for
both 48 kHz and 44.1 kHz sources — the 44.1 kHz case that used to starve now
plays.

### Latency: why the on-Mac test needs ~50 ms (measured — it's jitter, not drift)

The receiver logs the jitter-buffer **fill ratio** (0=empty, 1=full) and the
DLL's **measured device rate** every ~5 s. An idle low-latency sweep pinned the
cause:

| `latency_ms` | over/underruns (12 s) | fill swing | measured rate |
|---|---|---|---|
| 10 / 15 / 20 | ~138 / 0 | **0.04 .. 1.00** (hits the overrun edge) | **48000.0 Hz** |
| 50 | **0 / 0** | **0.42 .. 0.44** (tight, centered) | 48000.0 Hz |

The measured rate is **dead-on 48000 Hz at every latency**, so there is *no*
clock drift or rate surplus. The over/underruns are purely **arrival-timing
jitter**: `tools/aoo_send` paces blocks with `sleep_until`, which macOS timer-
coalesces into bursts, so packets arrive in clumps that swing the buffer across
its whole range. Only a ~50 ms buffer is deep enough not to overflow on a burst
(then the DLL settles the fill into a tight 0.42–0.44 band). Running the machine
idle didn't help, so the jitter is the software pacing itself, not CPU contention.

**Open follow-up — the ≤20 ms target (§9).** This jitter is a property of the
*test sender*, not AOO or the real link. The production Windows sender is clocked
by the WASAPI capture interrupt (regular, hardware-timed), runs on a separate
machine from the receiver, and the LAN adds only bounded jitter — exactly the
regime AOO's "5–20 ms on local networks" guidance targets. Low-latency tuning
toward ≤20 ms should therefore be validated on the real wired Windows→Mac link;
en route, `tools/aoo_send` could be made burst-free (busy-wait pacing or a
real-time thread) to serve as a fairer on-Mac proxy.
