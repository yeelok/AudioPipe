# Requirement Spec: Low-Latency Audio Link (Windows PC → macOS)

## 1. Overview

Build a two-part system that transports audio from a Windows PC to a macOS machine over a local IP network (LAN), with low latency and bit-transparent quality. On the PC, audio is captured from a **virtual output device** that other Windows apps can route to individually. On the Mac, the received audio is exposed as a **virtual input device** that other macOS apps (Zoom, OBS, DAWs, etc.) can select as a microphone/input.

This is for **personal use only** — no app distribution, no installer, no driver signing for release. Test-signing mode / self-signed certs are acceptable on Windows.

```
[Windows apps] --route audio--> [Virtual Output Device] --capture--> [Sender]
                                                                         |
                                                                       UDP/LAN
                                                                         |
[macOS apps] <--select as input-- [Virtual Input Device] <--write-- [Receiver]
```

## 2. Goals & Non-Goals

### Goals
- Per-application audio routing on Windows into a virtual output device.
- Audio appears as a selectable input device on macOS.
- Lowest practical latency (target ~10–20 ms end-to-end on wired LAN).
- Bit-transparent quality: no unnecessary resampling, no clipping.
- Stable for multi-hour sessions (clock-drift handled, no cumulative glitching).
- Auto-start at system boot on both machines.

### Non-Goals
- WAN / internet transport (LAN only; design may not assume high jitter).
- Cross-platform UI, installers, code signing for distribution.
- Bidirectional audio (one direction only: PC → Mac).
- Codecs / compression (raw PCM only).

## 3. Technology Choices (fixed)

- **Language:** C++ (C++17 or later). Audio callbacks and network in C++.
- **Windows virtual device:** fork an existing open-source virtual audio driver (recommend evaluating **Scream** and **VirtualAudioDriver**). Do NOT write a kernel driver from scratch unless forking is infeasible.
- **macOS virtual device:** fork **BlackHole** (CoreAudio AudioServerPlugin, user-space, no kernel extension).
- **Transport:** UDP. Raw PCM. Custom lightweight header.
- **Resampler (drift correction only):** libsoxr (async/variable-rate mode) or equivalent high-quality async resampler.

## 4. Audio Format (fixed)

- **Sample format:** 32-bit float (IEEE float), internal end-to-end.
- **Sample rate:** 48,000 Hz.
- **Channels:** 2 (stereo).
- **Frame size:** configurable; default 96 samples/frame (2 ms). Allowed range 48–240 samples.
- **Mode:** Windows **shared mode** (NOT exclusive — exclusive breaks multi-app routing).
- No Windows Sonic / Spatial Sound processing. Transport the raw mix unaltered.
- Convert to a different format only at the final hand-off if a consuming device strictly requires it.

## 5. Windows Sender Requirements

5.1. Provide/expose a virtual audio **output** device visible in Windows Sound settings, routable per-application via Settings → Sound → Volume mixer / App volume.

5.2. Capture the audio rendered to that virtual device (the driver's render buffer) in shared mode.

5.3. Read audio in fixed frames; packetize and send via UDP to a configurable Mac IP:port.

5.4. Add per-packet header (see §7). Maintain a monotonic sequence number.

5.5. Configurable parameters via a config file (JSON or INI): destination IP, destination port, frame size, sample rate (default 48000), channels (default 2).

5.6. Run as a Windows **Service** (or Scheduled Task at startup) so it runs without user login.

5.7. Handle device start/stop and reconnection gracefully (no crash if the network is briefly down).

## 6. macOS Receiver Requirements

6.1. Provide a virtual audio **input** device (forked from BlackHole) selectable by other apps as a microphone/input.

6.2. Receive UDP packets, validate header, place audio frames into a jitter buffer, then a lock-free ring buffer feeding the virtual device's render callback.

6.3. **Structural choice:** prefer a **separate receiver process** writing into the BlackHole-fork device for the first working version (easier to debug), with the option to later embed the network code inside the plugin (hosted by `coreaudiod`, no separate process). Document both paths; implement separate-process first.

6.4. Run via **LaunchDaemon** (boot, no login) or LaunchAgent (login), configurable.

6.5. Configurable parameters via config file: listen port, expected sample rate/channels, jitter buffer depth.

## 7. Network Protocol

- **Transport:** UDP, unicast.
- **Packet structure:**
    - `uint32` magic / version
    - `uint16` sequence number (wraps)
    - `uint64` timestamp (sender sample-clock count or ns)
    - `uint16` frame count (samples per channel in this packet)
    - `uint8`  channels
    - `uint8`  format id (0 = float32)
    - payload: interleaved PCM samples (float32)
- One audio frame per packet (default), or small N frames per packet — configurable.
- Receiver detects loss/reorder via sequence number.

## 8. Reliability & Quality Handling

8.1. **Jitter buffer:** fixed depth (default 3 frames) on the receiver. Configurable. (Adaptive jitter buffering is an optional later enhancement, not required for v1.)

8.2. **Packet loss concealment:** on a missing sequence number, insert one frame of either silence or a repeat of the last good frame (repeat preferred). Log loss counts.

8.3. **Reordering:** late packets older than the current playout position are dropped.

8.4. **Clock-drift correction (REQUIRED):** PC and Mac sample clocks differ slightly; over time the receiver buffer drifts toward under- or over-run. Continuously measure buffer fill level and correct via an async high-quality resampler (libsoxr) applying a tiny variable ratio (e.g. ±0.1%) to keep the buffer centered. This must be smooth (no audible pitch steps) and must keep a multi-hour session glitch-free.

8.5. **No clipping:** float pipeline; do not hard-convert/clip unless a final device demands integer output, in which case dither + clamp at the last stage only.

## 9. Latency Budget (target, wired LAN)

| Stage | Target |
|---|---|
| Capture frame | 2–5 ms |
| UDP transit (wired) | <1 ms |
| Jitter buffer | 4–9 ms |
| Mac device period | 2–5 ms |
| **End-to-end** | **~10–20 ms** |

Sub-10 ms is a stretch goal achievable with 1–2 ms frames on wired LAN; not required.

## 10. Configuration & Operations

- Both sides read a human-editable config file (JSON or INI).
- Logging to a file: startup/shutdown, device state, packet loss count, drift-correction ratio, buffer under/overruns.
- Clean shutdown on signal; auto-restart via the service/daemon manager if the process dies.

## 11. Build & Targets

- **Windows:** WDK for the driver fork; MSVC for the sender service. Document how to enable test-signing mode and load the self-signed/test-signed driver.
- **macOS:** Xcode / clang. BlackHole fork built and installed to `/Library/Audio/Plug-Ins/HAL/`. Document the `coreaudiod` restart step.
- Provide build instructions and a README per platform.

## 12. Suggested Implementation Order (for the agent)

1. Fork BlackHole; build, install, confirm it appears as a macOS input device.
2. Fork the Windows virtual audio driver; build, load in test-signing mode, confirm it appears as an output and is routable per-app.
3. Implement the UDP transport (sender + receiver), raw PCM, fixed jitter buffer, **no** drift correction yet. Confirm audio flows end-to-end and is selectable in a Mac app.
4. Add sequence numbers, loss detection, and concealment.
5. Add clock-drift correction (libsoxr). Validate a multi-hour session stays glitch-free.
6. Add config files and logging.
7. Add auto-start: Windows Service/Task + macOS LaunchDaemon/Agent.
8. (Optional) Merge the receiver network code into the BlackHole fork to remove the separate process.

## 13. Acceptance Criteria

- A specific Windows app's audio can be routed to the virtual output device while other apps stay on the default device.
- That audio is audible/selectable as an input on the Mac in at least one third-party app (e.g. QuickTime, Zoom, or OBS).
- Measured end-to-end latency on wired LAN ≤ 20 ms.
- No audible quality loss versus the source (bit-transparent path, 48 kHz / 32-bit float).
- A 2-hour continuous session runs with no audible glitches and no cumulative drift.
- Both processes start automatically after a reboot without user login.