# Windows handoff — start here (fresh Claude Code session)

You're picking up AudioPipe on the **Windows PC**. A previous session migrated the
project's transport to the **AOO** library (`AooSource`/`AooSink`) and validated
the **macOS** half end to end. The **Windows sender + tests now build and are
locally validated** on this PC (Part A below — done 2026-06-08; the build fixes + a
1 ms-timer overflow fix are in `main`). What remains: a real **Windows→Mac** link,
the **`driver-win`** device (Part B), and the wired ≤20 ms + 2-hour acceptance (Part C).

## Read these first
1. `docs/windows-bringup.md` — the step-by-step build/validate checklist (do this).
2. `docs/plan.md` → *"Addendum: migration to the AOO library"* — what changed and why.
3. `sender/README.md` — sender build/run/service specifics.

## What changed in the migration (the facts that matter here)
- Transport is now **AOO** (Audio over OSC, https://git.iem.at/aoo/aoo), bare
  `AooSource`/`AooSink` with our own sockets (AOO's NET/discovery layer is off).
  PCM float32 (Opus off) → bit-transparent. Pulled via CMake **FetchContent**,
  pinned to `v2.0-pre4`.
- **Gone:** the hand-rolled packet/jitter/drift modules, **libsoxr**, vcpkg, and
  the old `loopback_harness`/`drift_sim`/`udp_send` tools.
- **Sender** (`sender/sender_app.cpp`): WASAPI loopback capture → `AooSource` →
  UDP via Winsock. **Now compiles and runs on Windows** (the build fixes + a
  1 ms-timer overflow fix are in `main` — see "Trouble spots" below).
- **Driver** (`driver-win/`): `apply_48k.ps1` pins the virtual output to 48 kHz.
  Now **recommended, not required** — the AOO sink resamples any source rate, so
  a non-48 kHz endpoint no longer starves the device.

## Your task order
**Part A — sender over the network, no driver yet** (isolate transport from driver):
1. `git checkout main && git pull` (latest is the AOO migration + docs).
2. Build:
   ```bat
   cmake -S . -B build
   cmake --build build --config Release
   ```
   First configure clones AOO/json/Catch2 — needs **git + network**. No vcpkg/soxr.
3. In `config\sender.json`: set `dest_ip` to the Mac's LAN IP, `dest_port` 4010,
   `device_name` `""` (capture the default output).
4. Run: `build\sender\Release\audiopipe_sender.exe --console --config config\sender.json`
5. ✓ Success = sender logs `capturing ... at <rate> Hz`; the Mac receiver logs
   `aoo: stream start` then a `buffer fill swing .. measured device rate ~48000 Hz`
   heartbeat; you hear Windows audio on the Mac.

**Part B — the AudioPipe virtual output device** (see `driver-win/README.md`):
clone upstream → `apply_rename.ps1` → `apply_48k.ps1` → `build.ps1` → test-sign +
`trust_and_install.ps1` → set `device_name` to `"AudioPipe"`, route per-app in
Volume mixer.

**Part C — acceptance:** per-app routing, bit-transparency, **≤20 ms on wired
Ethernet** (§9), **2-hour soak** (§8). These belong here — the on-Mac loopback was
a pessimistic, jittery proxy.

## Environment you need
- Visual Studio 2022 + *Desktop development with C++* (MSVC + CMake ≥ 3.20).
- git + network (FetchContent). **No vcpkg, no libsoxr.**
- For Part B: the **WDK** matching your SDK + the WDK VS extension; test-signing
  (a reboot; desktop watermark).

## The other half (on the Mac — you can't see it from here)
- Mac LAN IP: `ipconfig getifaddr en0`.
- Receiver must be running: `build/receiver/audiopipe_receiver --config config/receiver.json`.
- UDP **4010** must be allowed through the macOS firewall.

## Trouble spots (fixed 2026-06-08, in `main`)
- **Send-buffer overflow → dropouts (the important one).** At Windows' default
  ~15.6 ms timer granularity the capture poll-loop (`Sleep`) and the net-thread
  drain (`recvfrom` timeout) fired too coarsely, so WASAPI bursts overran AOO's
  ~25 ms send queue — a flood of `AooSource: send buffer overflow`. Fix: request
  1 ms resolution via `timeBeginPeriod` (RAII guard, link `winmm`) + drop the drain
  timeout 5 ms → 2 ms. Result: 0 overflows, steady 500 blocks/s, holds under full
  CPU load.
- **MSVC build fixes:** `AooFormatPcm_init` takes an `AooInt32` rate (not the
  `AooSampleRate` double that `setup()` uses); the cross-platform
  `tests/test_aoo_loopback.cpp` needed winsock2/ws2tcpip (+`NOMINMAX`, `WSAStartup`,
  link `ws2_32`) and a local `kPi` (MSVC's `<cmath>` has no `M_PI`).
  `wasapi_capture.cpp`'s `propkeydef.h` ordering fix was already in place. The
  `AooSendFunc` 6-param signature, `<winsock2.h>`-before-`<windows.h>` order, and
  `SOCKET`/`char*` casts were all fine as written.
- AOO builds clean on Windows/MSVC via FetchContent (no CMake hiccups seen).
- If the sender exits with *"device mix format is not 32-bit float"*: the shared
  mix format must be float (it is by default) — don't use exclusive mode.

### Follow-up fixes (post-handoff, also in `main`)
- **Send-buffer overflow / "block xrun" — root cause + config fix.** The 1 ms
  timer above stopped most overflows; the underlying cause is that AOO's source
  send queue defaults to **25 ms** (`AOO_SOURCE_BUFFER_SIZE`) and we never raised
  it, so a burst overruns it → `process()` returns `kAooErrorOverflow` and calls
  `add_xrun()`, which the sink reports as `block xrun`. Fix:
  `source->setBufferSize(0.1)` (100 ms headroom, drains promptly, no added
  latency). **Not an AOO fork** — overflow is transient backpressure by AOO's own
  design. The self-heal stream-restart stays as a safety net.
- **AOO log routing.** `aoo_initialize` now installs an `AooSettings.logFunc` that
  forwards AOO's internal warnings/errors into our `Logger` (prefixed
  `aoo[warn]`/`aoo[error]`) — important for the service, which has no stderr.

## Reading the logs (success vs trouble)
- Receiver heartbeat: `buffer fill swing <min>..<max>  measured device rate <Hz>`.
  - fill swing reaching `..1.00` → overruns → raise `latency_ms` in `config/receiver.json`.
  - `measured device rate` far from 48000 → a real clock issue (investigate before
    tuning latency).
- `aoo: buffer UNDERRUN`/`OVERRUN` events = dropouts.

## State at handoff
- Branch `main`. macOS: e2e clean at 48 k and 44.1 k, 11/11 unit tests, a 20-min
  soak held 0 over/underruns with a rock-steady 48000 Hz measured rate.
- **Windows: sender + tests build (VS18 / MSVC 19.51), 11/11 tests pass, and the
  sender captures the default output at 48 k/float → AOO/UDP overflow-free**
  (validated against a local UDP listener — real Mac e2e not yet run from this PC).
- Latency note: the on-Mac loopback needed ~50 ms due to single-box scheduling
  jitter (not clock drift, not sender pacing — both were tested). Low-latency
  tuning toward ≤20 ms belongs on this real wired link. See `docs/plan.md`.
