# AudioPipe Windows sender

Captures audio rendered to a virtual output device via **WASAPI shared-mode
loopback** (spec §5.2) and streams it to the Mac receiver through an **AOO**
`AooSource` over UDP. AOO owns the wire protocol, timing, and clock-drift
handling; the source streams at the captured rate and the Mac sink resamples to
48 kHz, so a non-48 kHz endpoint no longer glitches. Reuses `core/` for config
and logging. Runs as a **Windows Service** (§5.6) or in the foreground for testing.

> Build and run this on the **Windows PC**. The code is part of the shared CMake
> project but compiles only under MSVC (it uses WASAPI / Winsock).

## Prerequisites

- Visual Studio 2022 (MSVC) + CMake ≥ 3.20
- Network access on first configure (CMake fetches nlohmann/json, Catch2, and
  the AOO library). No vcpkg/system audio libs are needed — AOO is built from
  source straight into the exe.

## Build

From the repo root:

```bat
cmake -S . -B build
cmake --build build --config Release
```

Output: `build\sender\Release\audiopipe_sender.exe`. (Add
`-DAUDIOPIPE_BUILD_TESTS=OFF` for a sender-only build that skips Catch2.)

## Configure

Edit `config\sender.json`:

```json
{
  "dest_ip": "192.168.1.50",   // the Mac's LAN IP
  "dest_port": 4010,            // must match the receiver
  "frame_samples": 96,
  "device_name": "AudioPipe"    // render device to capture (substring match)
}
```

`device_name` is matched as a substring of the render device's friendly name;
leave it `""` to capture the **default** output device. You can develop the
sender against your normal speakers or an existing virtual cable before the
`driver-win/` fork is ready (see [driver-win/README.md](../driver-win/README.md)).

## Run (foreground, for testing)

```bat
build\sender\Release\audiopipe_sender.exe --console --config config\sender.json
```

Route a specific app's audio to the virtual device in **Settings → System →
Sound → Volume mixer** (per-app output device = AudioPipe), start the Mac
receiver, and you should hear it on the Mac.

## Install as a service (§5.6, §10 auto-restart)

The exe statically links AOO, so it's self-contained — no runtime DLL to place
next to it. A Windows service reads `config\sender.json` from **its own folder**,
so the install script copies the exe and config into an install dir, registers an
auto-start service, and configures crash auto-restart.

From an **elevated** PowerShell:

```powershell
.\scripts\install_sender_service.ps1            # deploy to C:\AudioPipe + register
# edit C:\AudioPipe\config\sender.json -> set dest_ip to the Mac, then:
sc start AudioPipeSender
```

The service is `start= auto` (boot, no login) and is configured via
`sc failure` to restart 5 s after a crash, up to 3 times/day (§10). Logs go to
the `log_file` path in the config. To remove it:

```powershell
.\scripts\uninstall_sender_service.ps1 -Purge   # stop, delete, remove C:\AudioPipe
```

> Requires the **VC++ x64 runtime** (`MSVCP140`/`VCRUNTIME140*`) on the machine —
> already present wherever Visual Studio is installed; otherwise install the
> Microsoft Visual C++ Redistributable (x64). The UCRT ships with Windows 10.

Manual equivalent (if you prefer raw `sc`): copy `audiopipe_sender.exe` +
a `config\sender.json` into `C:\AudioPipe\`, then
`sc create AudioPipeSender binPath= "C:\AudioPipe\audiopipe_sender.exe" start= auto`
(the space after `binPath=` is required), and
`sc failure AudioPipeSender reset= 86400 actions= restart/5000/restart/5000/restart/5000`.

## Notes / limitations (v1)

- Requires the captured device's mix format to be 32-bit float at the configured
  rate (the AudioPipe virtual device is 48 kHz float). Other formats are rejected
  with a clear error.
- Sends packets as audio is captured. Long silence on the source produces no
  packets; the receiver conceals the gap. Continuous-silence keep-alive is a
  possible later refinement.
