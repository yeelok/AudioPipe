# Windows bring-up checklist (post-AOO)

The AOO migration changed the Windows half (sender now uses `AooSource`; the
driver is pinned to 48 kHz) but **none of it has been compiled on Windows yet** —
this is the project's only fully-untested surface. Work through this on the
Windows PC. Do **Part A first** (sender against your normal speakers) so you
validate the network path before taking on the kernel-mode driver.

Legend: ☐ = do it, ✓ = expected result.

## 0. Prerequisites
- ☐ **Visual Studio 2022** with *Desktop development with C++* (gives MSVC + CMake).
- ☐ **Network access** on first CMake configure — AOO, nlohmann/json and Catch2
  are fetched via `FetchContent` (needs `git`). No vcpkg / libsoxr anymore.
- ☐ Know the **Mac's LAN IP** (`ipconfig getifaddr en0` on the Mac) and that the
  Mac receiver is reachable on **UDP 4010** (open the macOS firewall if on).
- ☐ (Part B only) The **WDK** matching your SDK + the WDK VS extension, and a
  machine you'll put in **test-signing** mode (reboot, desktop watermark).

---

## Part A — Sender over the network (no driver yet)

This proves the AOO sender end-to-end while capturing your **default output**, so
a driver problem can't be confused with a transport problem.

1. ☐ **Build the sender** (from the repo root):
   ```bat
   cmake -S . -B build
   cmake --build build --config Release
   ```
   ✓ `build\sender\Release\audiopipe_sender.exe` exists. (First configure clones
   AOO into `build\_deps` — needs git/network.) Add `-DAUDIOPIPE_BUILD_TESTS=OFF`
   to skip Catch2 for a sender-only build.

2. ☐ **Point it at the Mac.** In `config\sender.json` set `dest_ip` to the Mac's
   IP, `dest_port` 4010, and `device_name` to `""` (capture the default output).

3. ☐ **Start the Mac receiver** (on the Mac):
   ```sh
   build/receiver/audiopipe_receiver --config config/receiver.json
   ```

4. ☐ **Run the sender** (Windows, console mode) and play any audio:
   ```bat
   build\sender\Release\audiopipe_sender.exe --console --config config\sender.json
   ```
   ✓ Sender logs `capturing 'default render device' at <rate> Hz, 2ch`.
   ✓ Mac receiver logs `aoo: stream start`, then the ~5 s heartbeat
     `buffer fill swing <min>..<max> measured device rate ~48000 Hz`.
   ✓ You hear the Windows audio on the Mac (select **AudioPipe** as input in
     QuickTime → New Audio Recording, or watch the level in System Settings →
     Sound → Input).

5. ☐ **Sanity-check the logs:**
   - ✓ The fill swing should **not** sit at `..1.00` (that = overruns → raise
     `latency_ms` in `config/receiver.json`).
   - ✓ `measured device rate` ~48000 Hz means no clock-rate surprise.
   - ✓ If your default output is 44.1 kHz, it still works (AOO resamples) — the
     sender just logs a non-48000 capture rate. That's the bug that's now fixed.

> If the sender exits with *"device mix format is not 32-bit float"*: the shared
> mix format must be float (it is by default). Don't put the endpoint in
> exclusive mode.

---

## Part B — Virtual output device "AudioPipe"

Now build the per-app-routable virtual speaker. Full detail in
[../driver-win/README.md](../driver-win/README.md); the sequence is:

6. ☐ Clone upstream + apply both patches:
   ```powershell
   cd driver-win
   git clone https://github.com/VirtualDrivers/Virtual-Audio-Driver.git upstream
   .\apply_rename.ps1     # rename endpoint -> AudioPipe, lower OS floor
   .\apply_48k.ps1        # pin to 48 kHz (recommended; AOO resamples otherwise)
   ```
7. ☐ `.\build.ps1` → ✓ `VirtualAudioDriver.sys/.inf/.cat` in `upstream\x64\Release\package\`.
8. ☐ `bcdedit /set testsigning on` → **reboot** → (elevated) `.\trust_and_install.ps1`.
9. ☐ ✓ **AudioPipe** appears under *Settings → Sound* as an output device.
   ✓ *AudioPipe → Properties → Advanced* offers **only 48000 Hz** (confirms `apply_48k`).

10. ☐ Set `config\sender.json` `device_name` to `"AudioPipe"`, restart the sender,
    and route a specific app to AudioPipe in **Volume mixer** (per-app output).
    ✓ Only that app's audio streams to the Mac; everything else stays local.

---

## Part C — Acceptance

11. ☐ **Per-app routing** (spec §5.1/§13): one app → AudioPipe, others unaffected.
12. ☐ **Bit-transparency** (§4): with the endpoint at 48 kHz, AOO does no
    resampling. (Cross-rate still works, just not bit-exact.)
13. ☐ **Latency ≤ 20 ms on wired Ethernet** (§9): measure on a wired link, not
    Wi-Fi. Lower `config/receiver.json` `latency_ms` toward 5–20 ms; watch the
    receiver fill-swing heartbeat stay off `1.00`. (See the latency analysis in
    [plan.md](plan.md) — on real hardware the jitter is far lower than the
    on-Mac software-paced test, so low latency should hold.)
14. ☐ **2-hour soak** (§8.x): run continuously; ✓ zero `buffer UNDERRUN`/`OVERRUN`
    events and a steady `measured device rate` in the receiver log.

15. ☐ **Auto-start as a service** (elevated):
    ```powershell
    .\scripts\install_sender_service.ps1 -Start
    ```
    ✓ Deploys exe + config to `C:\AudioPipe\` (no soxr.dll needed anymore) and
    registers an auto-start, crash-restarting service.

---

## Quick failure map
| Symptom | Likely cause |
|---|---|
| CMake configure fails fetching AOO | no network/git on the build box |
| Sender: "device mix format is not 32-bit float" | endpoint in exclusive mode / odd format |
| No audio, receiver shows no `stream start` | wrong `dest_ip`, or UDP 4010 blocked by the Mac firewall |
| Receiver fill swing pinned at `..1.00` | latency too low for the link → raise `latency_ms` |
| `measured device rate` far from 48000 | a real clock problem — investigate before tuning latency |
| AudioPipe missing after install | test-signing off, or no reboot, or cert not trusted (`trust_and_install.ps1`) |
| AudioPipe Advanced shows 44.1/96/192 kHz | `apply_48k.ps1` not applied before `build.ps1` |
