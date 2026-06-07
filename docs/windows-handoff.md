# Windows handoff — start here (fresh Claude Code session)

You're picking up AudioPipe on the **Windows PC**. A previous session migrated the
project's transport to the **AOO** library (`AooSource`/`AooSink`) and validated
the **macOS** half end to end. The **Windows half has not been compiled since the
migration** — building and validating it is the job now. This is the project's
only fully-untested surface.

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
  UDP via Winsock. Written to mirror the validated macOS usage, but **never
  compiled on Windows** — expect build fixes.
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

## Known likely-trouble spots (already hit / suspected)
- `sender/sender_app.cpp` is untested MSVC code — expect compile fixes.
  - `AooSendFunc` takes **6** params; the last is `AooFlag` (the send callback's
    signature must match exactly).
  - `<winsock2.h>` must be included **before** `<windows.h>` (already ordered so).
  - `sendto`/`recvfrom` use `SOCKET` + `char*` + `int` on Winsock (cast from
    `AooByte*`).
- AOO builds on Windows/MSVC (SonoBus ships it there), but if CMake/FetchContent
  hiccups, look there first.
- If the sender exits with *"device mix format is not 32-bit float"*: the shared
  mix format must be float (it is by default) — don't use exclusive mode.

## Reading the logs (success vs trouble)
- Receiver heartbeat: `buffer fill swing <min>..<max>  measured device rate <Hz>`.
  - fill swing reaching `..1.00` → overruns → raise `latency_ms` in `config/receiver.json`.
  - `measured device rate` far from 48000 → a real clock issue (investigate before
    tuning latency).
- `aoo: buffer UNDERRUN`/`OVERRUN` events = dropouts.

## State at handoff
- Branch `main`, all pushed. macOS: e2e clean at 48 k and 44.1 k, 11/11 unit tests,
  a 20-min soak held 0 over/underruns with a rock-steady 48000 Hz measured rate.
- Latency note: the on-Mac loopback needed ~50 ms due to single-box scheduling
  jitter (not clock drift, not sender pacing — both were tested). Low-latency
  tuning toward ≤20 ms belongs on this real wired link. See `docs/plan.md`.
