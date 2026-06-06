# AudioPipe macOS receiver

Receives §7 UDP packets, runs them through the portable `core/` pipeline (jitter
buffer → drift-corrected resampler → lock-free ring), and plays the result into
the **AudioPipe** virtual input device (the BlackHole fork in `driver-mac/`) via a
CoreAudio HAL output unit. Apps select "AudioPipe" as a microphone/input to hear
the Windows stream. Separate process per spec §6.3.

Build it with the rest of the project (`cmake --build build` →
`build/receiver/audiopipe_receiver`). It needs the AudioPipe device installed
first — see [../driver-mac/README.md](../driver-mac/README.md).

## Configure

`config/receiver.json` (spec §6.5):

```json
{
  "listen_port": 4010,        // must match the sender's dest_port
  "channels": 2,
  "jitter_frames": 3,         // §8.1 buffer depth
  "device_name": "AudioPipe", // CoreAudio device to play into
  "log_file": "audiopipe-receiver.log"
}
```

## Run (foreground)

```sh
build/receiver/audiopipe_receiver --config config/receiver.json
```

It logs `audio unit started (...)`, then `net: stream started` and a periodic
`net: pkts=… concealed=… drift_ratio=… ring=…` heartbeat. Select **AudioPipe**
as the input in QuickTime / System Settings → Sound to hear it.

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

> The receiver links **libsoxr** (Homebrew). Its dylib uses an absolute install
> name, so the launchd job resolves it without a special PATH. If you installed
> soxr somewhere non-standard, confirm with `otool -L /usr/local/bin/audiopipe_receiver`.
