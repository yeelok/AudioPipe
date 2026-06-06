# AudioPipe virtual audio device (Windows)

The Windows virtual **output** device is a fork of
[Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver)
(MIT, based on Microsoft's modern **Sysvad** sample). Apps route their audio to
this device per-application (spec §5.1); the [sender](../sender/README.md) then
loopback-captures it and streams it to the Mac.

> Why this base (not Scream): Scream does its networking **in the kernel**, which
> fights this project's userspace-sender architecture (spec §3/§5.3). Virtual-
> Audio-Driver is a clean virtual endpoint we capture from userspace, keeping all
> transport logic in the portable `core/`. See `docs/plan.md` for the full
> comparison.

Unlike the macOS BlackHole fork (customized purely via build flags), this driver
is forked by cloning upstream and applying a small rename, because it must be
built with the WDK in Visual Studio.

## Prerequisites

- Visual Studio 2022 + **Windows Driver Kit (WDK)** matching your SDK
- A test machine (or VM) where you can enable test-signing — kernel/driver dev
  is smoother on real hardware

## 1. Clone upstream into this folder

```bat
cd driver-win
git clone https://github.com/VirtualDrivers/Virtual-Audio-Driver.git upstream
```

(Keeping it under `upstream/` makes it easy to pull updates and keep our rename
as a small, visible diff.)

## 2. Rename the device to "AudioPipe"

Open the solution in Visual Studio. Change the **render (speaker) endpoint
friendly name** to `AudioPipe` so it matches `device_name` in `sender.json`:

- In the driver's **`.inf`** — the `[Strings]` section device/endpoint name
  values (search the INF for the current product/endpoint name strings).
- In the **source** — the endpoint/topology name strings (grep the project for
  the existing device name, e.g. its product string, and replace with
  `AudioPipe`).
- Optionally set a distinct hardware ID / component GUID so it installs
  alongside, not over, any existing install.

The sender matches `device_name` as a substring, so an exact match isn't
required — but a clean `AudioPipe` name is clearest.

## 3. Build

Build the driver project, **Release / x64**. You get `AudioPipe.sys` (or the
upstream name), its `.inf`, and a `.cat`.

## 4. Enable test-signing and install

Personal use only — test-signing is fine (spec §1, §11). As **Administrator**:

```bat
bcdedit /set testsigning on
:: reboot, then:
pnputil /add-driver path\to\AudioPipe.inf /install
```

If Windows requires a signed catalog even for test-signing, create a self-signed
test certificate and sign the `.sys`/`.cat` with `signtool` (see the upstream
repo's signing/install scripts, which automate this), then `pnputil` as above.

## 5. Confirm

- **Settings → System → Sound** lists **AudioPipe** as an output device.
- **Volume mixer** lets you set a specific app's output to AudioPipe while other
  apps stay on the default device (spec §13 acceptance).
- Point `sender.json` `device_name` at `AudioPipe` and run the sender.

## Uninstall

```bat
pnputil /delete-driver oemXX.inf /uninstall   :: find oemXX via: pnputil /enum-drivers
bcdedit /set testsigning off                  :: optional, then reboot
```

## Incremental path (no driver yet)

You don't need this fork to start testing the sender end-to-end: set
`device_name` to `""` (default output) or install a ready-made virtual cable
(e.g. VB-CABLE) and capture that. Finish the fork when you want clean per-app
routing into a device named AudioPipe.
