# AudioPipe virtual audio device (Windows)

The Windows virtual **output** device is a fork of
[Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver)
(MIT, based on Microsoft's modern **Sysvad** sample). Route any app's audio to
this device per-application (spec §5.1); the [sender](../sender/README.md) then
loopback-captures it and streams it to the Mac. So **only audio you route into
"AudioPipe" gets sent** — everything else keeps using your normal output.

> Why this base (not Scream): Scream does its networking **in the kernel**, which
> fights this project's userspace-sender architecture (spec §3/§5.3). Virtual-
> Audio-Driver is a clean virtual endpoint we capture from userspace, keeping all
> transport logic in the portable `core/`. See `docs/plan.md` for the comparison.

This is a **kernel-mode (WDK) driver**. Building/installing it is heavier than the
rest of the project: it needs the WDK, **test-signing enabled (a reboot)**, and
elevation. For personal use that's acceptable (spec §1/§11). The upstream clone
lives under `upstream/` (gitignored — not vendored); our only changes are two
small patch scripts (`apply_rename.ps1`, `apply_48k.ps1`) applied after cloning,
so it stays trivially re-pullable.

## Prerequisites

- **Visual Studio 2022+** with *Desktop development with C++*.
- The **Windows Driver Kit (WDK)** matching your installed SDK
  (<https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk>),
  plus the **WDK Visual Studio extension** so the driver build targets load.
  Without the WDK, `build.ps1` fails (the driver MSBuild targets are missing).
- A machine you're willing to put in **test-signing** mode (shows a desktop
  watermark; reversible).

## 1. Clone upstream + apply the AudioPipe rename

```powershell
cd driver-win
git clone https://github.com/VirtualDrivers/Virtual-Audio-Driver.git upstream
.\apply_rename.ps1        # rename endpoint to AudioPipe + lower OS floor
.\apply_48k.ps1           # pin the endpoint to 48 kHz only (see step 1b)
```

`apply_rename.ps1` edits `upstream\Source\Main\VirtualAudioDriver.inx` (the INF
template, UTF-16) and does two things:

1. Renames the **render/speaker** endpoint, device description, and service to
   **AudioPipe**. (The driver also exposes a capture/mic endpoint; we leave it
   named as upstream ships it — the sender uses the speaker, not the mic.) Pass
   `-DeviceName "Something Else"` for a different name; keep `sender.json`'s
   `device_name` in sync.
2. Lowers the `[Manufacturer]` **OS build floor** from `10.0...22000` (upstream
   targets Windows 11 only) to `10.0...17763` (Win10 1809+). Without this, on
   Windows 10 PnP reports *"no better matching drivers"* and binds the generic
   null driver instead of ours. Override with `-MinBuild`.

## 1b. Pin the endpoint to 48 kHz (recommended)

```powershell
.\apply_48k.ps1
```

> **Not required** since the macOS receiver moved to the AOO library: the
> `AooSink` resamples whatever rate the sender streams to the 48 kHz device, so a
> non-48 kHz endpoint no longer starves it (that earlier dropout bug is gone).
> You can skip this step and the link still works — handy during bring-up.

Pinning to 48 kHz is still **recommended** for the best path:

- **Bit-transparency** — at 48 kHz, source and device rates match and AOO does
  *zero* resampling (bit-exact, per spec §4). Unpinned at 44.1 kHz, AOO resamples
  44.1 → 48 kHz: high quality, but no longer bit-exact.
- **No double resample** — unpinned at 44.1 kHz the audio is resampled twice
  (Windows' shared mixer takes each app → 44.1 kHz, then AOO 44.1 kHz → 48 kHz).
  Pinning removes the second hop.
- **Bandwidth** — upstream advertises up to **192 kHz**; unpinned, Windows could
  pick 96/192 kHz as the mix format → 2–4× the UDP bandwidth for no benefit.

Upstream's speaker endpoint advertises **8 kHz – 192 kHz (including 44.1 kHz)**.
`apply_48k.ps1` edits `upstream\Source\Filters\speakerwavtable.h` so the speaker
advertises **48 kHz only** (16/24/32-bit, stereo) — the supported-formats array,
the modes array, and the KS data range are all narrowed to 48000 Hz, so Windows
has exactly one rate to pick. The script is idempotent, backs up the original to
`speakerwavtable.h.orig`, and throws if upstream's table shape changed (so it
never silently leaves a non-48k format in).

> You can confirm after install: **Settings → Sound → AudioPipe → Properties →
> Advanced** should offer only 48000 Hz formats.

## 2. Build (Release / x64)

```powershell
.\build.ps1
```

Finds MSBuild via `vswhere` and builds the solution. Output:
`upstream\x64\Release\package\` → `VirtualAudioDriver.sys`, `VirtualAudioDriver.inf`,
`virtualaudiodriver.cat`. A Release build **test-signs** the `.sys` and
generates+signs the `.cat` with an auto-created WDK test certificate
(`WDKTestCert <user>`).

`build.ps1` makes two adjustments so this builds on a normal dev box:

- It writes `upstream\Directory.Build.props` pinning the INF **DriverVer date**
  (default `01/01/2025`). StampInf uses the *local* date but `inf2cat` validates
  against *UTC*, so a machine ahead of UTC otherwise stamps "tomorrow" and the
  catalog build fails with *"postdated DriverVer not allowed"*.
- It passes `/p:SkipPackageVerification=true /p:ApiValidator_Enable=false` to skip
  the WDK's **universal-driver validators** (InfVerif / ApiValidator). These are
  compliance checks for Store/universal drivers, not needed for a locally
  test-signed personal driver — and they're broken/missing in some preview WDKs
  (e.g. `Unable to load DLL 'x86\InfVerif.dll'`, or `ApiValidator` exit 193).

## 3. Enable test-signing, then trust + install

A test-signed driver won't load until test-signing is on and the machine has
rebooted. From an **elevated** prompt:

```powershell
bcdedit /set testsigning on
:: reboot, then (elevated):
.\trust_and_install.ps1
```

`trust_and_install.ps1` extracts the certificate the build used to sign the
driver, adds it to LocalMachine **Root** + **TrustedPublisher** (so Windows
accepts it), and runs `pnputil /add-driver VirtualAudioDriver.inf /install`.
Because this is a **root-enumerated** software device, `pnputil` only publishes
the package — it doesn't create a device node — so the script then runs
`devgen` (WDK Tools) to create one (`root\VirtualAudioDriver`, instance id
`AudioPipe`). PnP binds the driver and **AudioPipe** appears under
*Settings → Sound*. (If `devgen` isn't found, create the node manually:
*Device Manager → Action → Add legacy hardware → install from the built INF*.)

> Execution policy: if PowerShell refuses to run the script
> (*"running scripts is disabled"*), unblock it for the current window only:
> `Set-ExecutionPolicy -Scope Process Bypass -Force`.

> If the build did **not** sign the `.sys` (some WDK setups default `SignMode=Off`),
> the script says so. In that case self-sign before installing: create a code-
> signing cert (`New-SelfSignedCertificate -Type CodeSigningCert -CertStoreLocation
> Cert:\LocalMachine\My -Subject "CN=AudioPipe Test"`), then
> `signtool sign /fd SHA256 /a /n "AudioPipe Test" ...\virtualaudiodriver.cat`
> (sign the `.cat`, which vouches for the `.sys`), then re-run the install.

## 3b. Make the device persist across reboots (recommended)

`trust_and_install.ps1` creates the device with **`devgen`**, whose software
devices are **boot-scoped** — after a reboot the node becomes a `CM_PROB_PHANTOM`
remnant and "AudioPipe" disappears from Sound (the driver package and cert
survive; only the node is gone). To create a node that **persists across
reboots**, run `install_persistent.ps1` instead — it removes any devgen/stale
node and uses `devcon install` to create a root-enumerated device:

```powershell
.\install_persistent.ps1        # elevated; root-enumerated device, survives reboot
```

> Persistence still requires test-signing to stay on (the Test Mode watermark).
> To set up the device **and** the sender Windows Service in one shot, use
> [`../scripts/install_all.ps1`](../scripts/install_all.ps1).

## 4. Confirm + wire up the sender

- **Settings → System → Sound** lists **AudioPipe** as an output device.
- **Volume mixer** lets you set a specific app's output to AudioPipe while other
  apps stay on the default device (spec §13 acceptance).
- Point the sender at it and run:

```powershell
:: in config\sender.json:  "device_name": "AudioPipe"  (substring match)
build\sender\audiopipe_sender.exe --console --config config\sender.json
```

Now only the apps you route to **AudioPipe** are captured and streamed to the Mac.

## Uninstall

```powershell
pnputil /enum-drivers                          :: find the oemNN.inf for AudioPipe
pnputil /delete-driver oemNN.inf /uninstall    :: (elevated)
bcdedit /set testsigning off                   :: optional; reboot to apply
```

## Incremental path (no driver yet)

You don't need this fork to test the pipeline end-to-end: set `sender.json`
`device_name` to `""` (default output) or install a ready-made virtual cable
(e.g. VB-CABLE) and capture that. Build this fork when you want a clean,
per-app-routable output literally named **AudioPipe**.

## Files here

| File | What |
|---|---|
| `apply_rename.ps1` | Rename the cloned driver's speaker endpoint to AudioPipe |
| `apply_48k.ps1` | Pin the speaker endpoint to 48 kHz only (formats, modes, data range) |
| `build.ps1` | Build Release/x64 via MSBuild (needs the WDK) |
| `trust_and_install.ps1` | Trust the test cert + `pnputil` install via `devgen` (elevated; node is **boot-scoped**) |
| `install_persistent.ps1` | Same, but creates a **persistent** root device via `devcon` (survives reboot, elevated) |
| `upstream/` | The cloned fork (gitignored; created by step 1) |
