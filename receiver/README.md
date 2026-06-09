# AudioPipe macOS receiver

Receives an **AOO** stream over UDP and plays it into the **AudioPipe** virtual
input device (the BlackHole fork in `driver-mac/`) via a CoreAudio HAL output
unit. Apps select "AudioPipe" as a microphone/input to hear the Windows stream.
Separate process per spec §6.3.

The transport is the [AOO](https://git.iem.at/aoo/aoo) library (`AooSink`): it
owns the jitter buffer, time-DLL clock-drift correction, **dynamic resampling**
(so a non-48 kHz source is resampled to the device rate instead of starving it),
and optional packet resend. The CoreAudio render callback calls `AooSink::process()`;
a network thread feeds `handleMessage()` and flushes `send()`.

Build it with the rest of the project (`cmake --build build` →
`build/receiver/audiopipe_receiver`). It needs the AudioPipe device installed
first — see [../driver-mac/README.md](../driver-mac/README.md).

## Configure

`config/receiver.json`:

```json
{
  "listen_port": 4010,        // must match the sender's dest_port
  "channels": 2,
  "latency_ms": 50,           // AOO playout latency (jitter buffer)
  "device_name": "AudioPipe", // CoreAudio device to play into
  "log_file": "audiopipe-receiver.log"
}
```

`latency_ms` is the playout cushion. **50 ms is glitch-free out of the box**; on
a clean wired LAN you can lower it toward 5–20 ms (AOO's recommended range for
local networks) to meet the spec's ≤20 ms target. Override at runtime without a
rebuild via the `AUDIOPIPE_LATENCY_MS` env var (and `AUDIOPIPE_BUFFER_MS` for the
network buffer, which defaults to 4× latency).

## Run (foreground)

```sh
build/receiver/audiopipe_receiver --config config/receiver.json
```

It logs `audio unit started (...)`, then AOO `stream start` / `format change`,
any `buffer UNDERRUN`/`OVERRUN` events (your dropout signal), and a ~5 s
heartbeat: `buffer fill swing <min>..<max>  measured device rate <Hz>`. A swing
that reaches ~1.00 means the jitter buffer is overflowing — raise `latency_ms`;
the measured device rate exposes any real clock drift (it should sit at ~48000).
AOO's own internal warnings/errors are routed into this log too, prefixed
`aoo[warn]` / `aoo[error]` (via `AooSettings.logFunc`).
Select **AudioPipe** as the input in QuickTime / System Settings → Sound to hear it.

## Auto-start at boot (§6.4)

`install_launchd.sh` installs the binary + config to stable paths
(`/usr/local/bin`, `/usr/local/etc/audiopipe`) and registers a launchd job with
`RunAtLoad` + `KeepAlive` (starts automatically, auto-restarts if it dies).

```sh
./receiver/install_launchd.sh            # per-user LaunchAgent (recommended)
./receiver/install_launchd.sh --daemon   # system LaunchDaemon (boot, no login)
./receiver/uninstall_launchd.sh [--daemon] [--purge]
```

**Agent vs daemon — important.** CoreAudio device access is reliable inside a
user GUI session, which is where a **LaunchAgent** runs. So the agent is the
recommended default; to get "starts after reboot with no manual login," combine
it with macOS auto-login (System Settings → Users & Groups → Automatically log
in as…).

A **LaunchDaemon** (`--daemon`) starts at boot with no user session, which
satisfies §6.4's "boot, no login" literally — but a daemon has no user audio
session, so opening the AudioPipe HAL device may fail depending on macOS version.
Only use `--daemon` if you've confirmed audio actually plays that way.

Check status / logs:

```sh
launchctl print gui/$(id -u)/com.audiopipe.receiver | grep -E 'state|pid'   # agent
tail -f ~/Library/Logs/audiopipe-receiver.err.log
```

> The receiver statically links the AOO library (fetched at configure time and
> built into the binary), so the launchd job has no extra runtime dylib to
> resolve. Confirm linkage with `otool -L /usr/local/bin/audiopipe_receiver`.
