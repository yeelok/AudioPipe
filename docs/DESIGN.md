# AudioPipe — Design & Implementation

The detailed reference for the project **as built**: requirements, architecture,
the AOO-based transport, and the design decisions/findings. The top-level
[README](../README.md) is the overview + quick start; the per-component
`*/README.md` files cover build/run/install detail.

> Source comments cite the section numbers used here (e.g. `§5.2`, `§8.4`). The
> numbering follows the original requirements; where AOO changed *how* a
> requirement is met, it's noted inline.

---

## 1. Overview

Transport audio from a **Windows PC → macOS** over a LAN, low-latency and
bit-transparent. On Windows, audio is captured from a **virtual output device**
that apps route to individually; on macOS the received audio is a **virtual input
device** other apps select as a microphone. **Personal use only** — no
distribution, no release signing (test-signing / ad-hoc is fine).

```
Windows app ─▶ AudioPipe output device (driver-win) ─▶ sender: WASAPI loopback ─▶ AooSource
     └─ UDP / LAN ─▶ receiver: AooSink ─▶ AudioPipe input device (driver-mac) ─▶ Mac app
```

## 2. Goals & non-goals

**Goals:** per-app routing on Windows; selectable input on macOS; ~10–20 ms
end-to-end on wired LAN; bit-transparent (no unnecessary resampling/clipping);
multi-hour stability (clock drift handled); auto-start at boot on both machines.

**Non-goals:** WAN transport; UI/installers/release signing; bidirectional audio;
codecs/compression (raw PCM only).

## 3. Technology choices (as built)

- **Language:** C++17.
- **Send/receive engine:** the [**AOO**](https://git.iem.at/aoo/aoo) (Audio over
  OSC) library — bare `AooSource`/`AooSink`, our own UDP sockets, discovery layer
  off. PCM float32 (Opus off) → bit-transparent. Fetched via CMake `FetchContent`
  (Standard Improved BSD), pinned to `v2.0-pre4`.
  *(This replaces the original plan of a custom UDP header + a libsoxr drift
  resampler: AOO provides the wire protocol, jitter buffer, clock-drift correction
  and packet resend — see [§7](#7-transport-aoo), [§8](#8-reliability--quality),
  and the [migration appendix](#appendix--aoo-migration--findings).)*
- **Windows virtual device:** a fork of **Virtual-Audio-Driver** (Sysvad-based) —
  see [§12](#12-driver-forks).
- **macOS virtual device:** a fork of **BlackHole** (user-space CoreAudio
  AudioServerPlugin, no kernel extension).

## 4. Audio format (fixed)

32-bit float, **48 kHz**, **stereo**, internal end-to-end. AOO block size is
configurable (`frame_samples`, default **96 = 2 ms**, range 48–240). Windows
**shared mode** (exclusive breaks per-app routing). No Sonic/Spatial processing —
the raw mix is transported unaltered.

## 5. Windows sender (`sender/`)

5.1 Exposes the **AudioPipe** virtual *output* device (the driver fork), routable
per-app via Settings → Sound → Volume mixer.
5.2 Captures that device's render via **WASAPI shared-mode loopback** (float32).
5.3–5.4 Feeds the captured audio to an **`AooSource`**, which packetizes + sends
over UDP (AOO owns the wire protocol; the original custom header is retired).
5.5 JSON config (`config/sender.json`): `dest_ip`, `dest_port`, `sample_rate`,
`channels`, `frame_samples`, `device_name`, `log_file`.
5.6 Runs as a **Windows Service** (`service.cpp`) or `--console`.
5.7 Resilient to a down/absent receiver: non-blocking UDP send, and
`SIO_UDP_CONNRESET` disabled so an ICMP "port unreachable" can't kill the socket.
AOO streams at the captured rate; the Mac sink resamples to 48 kHz, so a non-48 kHz
endpoint no longer glitches.

## 6. macOS receiver (`receiver/`)

6.1 Plays into the **AudioPipe** virtual *input* device (BlackHole fork).
6.2 UDP → **`AooSink`** (jitter buffer + drift correction) → CoreAudio HAL output
unit render callback.
6.3 **Separate process** (per the spec's recommended first path; embedding into
the plug-in is the optional later step).
6.4 Auto-start via **launchd** — LaunchAgent (login, reliable CoreAudio access) or
LaunchDaemon (boot). See [receiver/README.md](../receiver/README.md).
6.5 JSON config (`config/receiver.json`): `listen_port`, `sample_rate`,
`channels`, `latency_ms`, `device_name`, `log_file`.

## 7. Transport (AOO)

UDP, unicast, OSC-framed by AOO. We own the socket: a **network thread** calls
`AooSink/Source::send()` + `handleMessage()` + `pollEvents()`; the **audio thread**
calls `process()`. (These thread roles are AOO's documented contract — `process`
is RT-safe for the audio thread; `send`/`handleMessage`/`startStream` are
threadsafe for the network thread.) The original 18-byte custom packet header is
superseded by AOO's protocol.

## 8. Reliability & quality

8.1 **Jitter buffer:** AOO sink, depth set by `latency_ms` (playout latency);
buffer capacity decoupled via `setBufferSize` (default 4× latency in the receiver).
8.2/8.3 **Loss / reorder:** AOO conceals gaps and, with `setResendData`, requests
**resends** (NACK) — on a low-RTT LAN it recovers most dropped packets.
8.4 **Clock-drift correction:** AOO's **time-DLL filter + dynamic resampling**
(`setDynamicResampling`) — this replaces the spec's libsoxr ±0.1 % plan and, unlike
it, handles *arbitrary* rate differences (e.g. 44.1 kHz → 48 kHz), not just tiny
drift.
8.5 **No clipping:** float32 throughout.
**Send-buffer headroom:** `source->setBufferSize(0.1)` (100 ms; default is 25 ms)
so capture/scheduling bursts can't overflow the source queue — see the
[appendix](#appendix--aoo-migration--findings).

## 9. Latency

Target budget (wired LAN): capture 2–5 ms · UDP <1 ms · jitter buffer 4–9 ms ·
device period 2–5 ms → **~10–20 ms end-to-end**.

**Finding (measured):** the on-Mac loopback (sender stand-in + receiver on one
box) needs `latency_ms≈50` to stay glitch-free — *not* clock drift: the receiver's
DLL reports the device rate **dead-on 48000 Hz** at every latency, while the
jitter-buffer fill swings the full 0.04..1.00 at low latency and settles to a tight
0.42..0.44 at 50 ms. The cause is single-box scheduling jitter (sender process +
receiver net thread + CoreAudio thread + a software-clocked virtual device all
contending); making the test sender burst-free did **not** help. The **≤20 ms
target belongs on the real two-machine wired link**, where the sender is
hardware-clocked and the halves don't share a CPU.

## 10. Configuration & operations

Human-editable JSON both sides (see §5.5/§6.5). File logging: startup/shutdown,
device state, the receiver's `buffer fill swing … measured device rate …`
heartbeat (~5 s), and **AOO's own internal warnings/errors routed into the log**
via `AooSettings.logFunc` (so they survive the headless Windows service, which has
no stderr). Clean shutdown on signal; auto-restart via the service / launchd
`KeepAlive`.

## 11. Build & targets

- **Dev build + tests:** `cmake -S . -B build -G Ninja && cmake --build build &&
  ctest --test-dir build`. No system audio libs — AOO is built from source.
- **One-shot build + install:** `scripts/setup_macos.sh` and
  `scripts/setup_windows.ps1` (see the README quick start). They orchestrate the
  per-component build/install scripts.
- **Windows** needs MSVC + the WDK (driver) + test-signing; **macOS** needs Xcode/
  clang and installs the HAL plug-in to `/Library/Audio/Plug-Ins/HAL/`.

## 12. Driver forks

**Windows — Virtual-Audio-Driver (Sysvad), not Scream.** Scream does its
networking *in the kernel*, which fights this project's userspace-sender design and
can't be debugged/reused from `core/`. Virtual-Audio-Driver is a clean virtual
endpoint we capture from userspace.

| Factor | Virtual-Audio-Driver | Scream |
|---|---|---|
| Sample base | **Sysvad** (modern, MS-recommended) | MSVAD (older) |
| Networking | None — userspace ✅ | In kernel ❌ |
| Per-app routing | Yes (Volume mixer) ✅ | Not documented |
| License | MIT (+ MS-PL samples) ✅ | MS-PL |
| Fork effort | Low (mainly rename + pin 48 kHz) | High (kernel UDP rewrite) |

The fork is minimal: `apply_rename.ps1` (name → AudioPipe, lower the OS floor) +
`apply_48k.ps1` (pin the endpoint to 48 kHz — *recommended*, not required, since
the sink resamples; it keeps the path bit-exact and caps bandwidth). See
[driver-win/README.md](../driver-win/README.md).

**macOS — BlackHole**, customized at build time (name, bundle id, 2ch, 48 kHz,
ad-hoc signed). See [driver-mac/README.md](../driver-mac/README.md).

## 13. Acceptance criteria & status

**Criteria:** per-app routing on Windows; audible/selectable input on macOS in a
3rd-party app; **≤20 ms** on wired LAN; bit-transparent (48 kHz/float32); a
**2-hour** session with no glitches/drift; both sides auto-start after reboot.

**Status — done & validated:** AOO transport on both halves; macOS receiver
end-to-end clean at `latency_ms=50` for 48 kHz **and** 44.1 kHz sources; 11/11 unit
tests (macOS + Windows/MSVC); a 20-minute on-Mac soak with 0 over/underruns and a
steady measured rate; the Windows sender builds and streams overflow-free to a
local listener.

**Open:** the real two-machine Windows→Mac link on the AOO build; the **≤20 ms
wired** measurement; the **2-hour** end-to-end soak.

---

## Architecture (current)

```
            WINDOWS                         macOS
  ┌──────────────────────────┐    ┌──────────────────────────┐
  │ driver-win: AudioPipe     │    │ receiver process:         │
  │   virtual OUTPUT (48 kHz)  │    │   net thread:             │
  │ sender process:           │    │     recvfrom → AooSink     │
  │   audio thread:           │    │     .handleMessage         │
  │     WASAPI loopback →      │UDP │     .send/.pollEvents      │
  │     AooSource.process     │───▶│   CoreAudio render cb:     │
  │   net thread:             │    │     AooSink.process →      │
  │     AooSource.send/handle  │    │     driver-mac AudioPipe   │
  └──────────────────────────┘    │     virtual INPUT          │
                                   └──────────────────────────┘
```

`core/` is now small: config, file logging, and the 48 kHz/float32 constants
(`format.h`). AOO owns everything that used to be hand-rolled.

## Windows build & troubleshooting

Build **the sender against your default output first** (`device_name: ""`) to
validate the network path before taking on the kernel-mode driver. Common issues:

| Symptom | Likely cause |
|---|---|
| CMake configure fails fetching AOO | no network/git on the build box |
| `device mix format is not 32-bit float` | endpoint in exclusive mode / odd format |
| `aoo[warn]: … send buffer overflow` / receiver `block xrun` | bursts overran the source queue — mitigated by the 1 ms timer + `setBufferSize(0.1)`; raise further if it persists |
| No audio, receiver shows no `stream start` | wrong `dest_ip`, or UDP `listen_port` blocked by the Mac firewall |
| Receiver fill swing pinned at `..1.00` | latency too low for the link → raise `latency_ms` |
| `measured device rate` far from 48000 | a real clock problem — investigate before tuning latency |
| AudioPipe missing after install | test-signing off / no reboot / cert not trusted |
| AudioPipe Advanced shows 44.1/96/192 kHz | `apply_48k.ps1` not applied before `build.ps1` |

The MSVC build needs: `<winsock2.h>` before `<windows.h>`; `AooFormatPcm_init`'s
rate arg is `AooInt32` (not the `AooSampleRate` double `setup()` takes); link
`winmm` (timer), `ws2_32` (Winsock). These are already in the tree.

## Appendix — AOO migration & findings

**Why AOO.** The original `core/` hand-rolled the transport (a custom packet
codec, jitter buffer, PI drift controller, libsoxr resampler). It assumed every
stage ran at exactly 48 kHz with only a ±0.1 % drift trim, so a non-48 kHz Windows
endpoint (e.g. 44.1 kHz, very common) starved the playout ring → continuous
dropouts. Rather than build a full SRC + clock-sync layer, we adopted AOO (the
engine behind SonoBus): UDP + jitter buffer + **time-DLL drift correction +
dynamic resampling** + NACK resend, in tested form, BSD-licensed, float32/PCM.
Removed: `core/{packet,jitter_buffer,drift_control,resampler}` + libsoxr + the old
`loopback_harness`/`drift_sim`/`udp_send` tools. Kept: config, logging, format.

**Block-xrun = send-buffer overflow (a config fix, not a fork).** The receiver's
`block xrun` is the downstream symptom of the **sender's** send queue overflowing:
`AooSource::process()` calls `add_xrun()` on overflow, which the sink reports.
The source queue defaults to `AOO_SOURCE_BUFFER_SIZE = 25 ms`; a burst overruns it.
Fix: `source->setBufferSize(0.1)`. AOO's own comment ("clients are still supposed
to call send() to drain") confirms overflow is transient backpressure by design —
no AOO fork warranted.

**AOO examples.** AOO's bundled examples are minimal `AooClient` demos using fewer
controls than we do (we set dynamic resampling, resend, buffer sizing). The one
useful thing they don't show — `AooSettings.logFunc` — is now adopted (see §10).

**Offline pipeline check.** `tools/pipeline_check` runs a real WAV through an
in-process `AooSource → AooSink` (synthetic clock) with loss/reorder/drift
injection and writes a listenable output WAV — loss/reorder/cross-rate are faithful;
drift is understated by the single-process clock (same limitation as the unit test).
