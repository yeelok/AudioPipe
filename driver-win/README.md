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
lives under `upstream/` (gitignored — not vendored); our only change is a one-line
rename applied by `apply_rename.ps1`, so it stays trivially re-pullable.

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
..\driver-win\apply_rename.ps1        # or: .\apply_rename.ps1
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
| `build.ps1` | Build Release/x64 via MSBuild (needs the WDK) |
| `trust_and_install.ps1` | Trust the test cert + `pnputil` install (elevated) |
| `upstream/` | The cloned fork (gitignored; created by step 1) |
