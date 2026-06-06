# AudioPipe Windows sender

Captures audio rendered to a virtual output device via **WASAPI shared-mode
loopback** (spec §5.2), packetizes it (§7), and streams it over UDP to the Mac
receiver. Reuses the portable `core/` library for the wire format, config, and
logging. Runs as a **Windows Service** (§5.6) or in the foreground for testing.

> Build and run this on the **Windows PC**. The code is part of the shared CMake
> project but compiles only under MSVC (it uses WASAPI / Winsock).

## Prerequisites

- Visual Studio 2022 (MSVC) + CMake ≥ 3.20
- [vcpkg](https://github.com/microsoft/vcpkg) for libsoxr (pulled in by `core`):
  ```
  vcpkg install soxr
  ```

## Build

From the repo root, using the vcpkg toolchain:

```bat
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
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

## Install as a service (§5.6)

The service reads `config\sender.json` from **its own folder**, so place the exe
and a `config\` subfolder together (e.g. `C:\AudioPipe\`).

```bat
sc create AudioPipeSender binPath= "C:\AudioPipe\audiopipe_sender.exe" start= auto
sc start AudioPipeSender
:: ... later ...
sc stop AudioPipeSender
sc delete AudioPipeSender
```

(The space after `binPath=` is required by `sc`.) Logs go to the `log_file`
path in the config. The service auto-starts at boot without user login.

## Notes / limitations (v1)

- Requires the captured device's mix format to be 32-bit float at the configured
  rate (the AudioPipe virtual device is 48 kHz float). Other formats are rejected
  with a clear error.
- Sends packets as audio is captured. Long silence on the source produces no
  packets; the receiver conceals the gap. Continuous-silence keep-alive is a
  possible later refinement.
