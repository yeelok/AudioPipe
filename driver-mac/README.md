# AudioPipe virtual audio device (macOS)

A customized fork of [BlackHole](https://github.com/ExistentialAudio/BlackHole)
(GPL-3.0) — a user-space CoreAudio `AudioServerPlugin`, no kernel extension. It
presents a single **AudioPipe** device (48 kHz, 2ch, float32) that apps can
select as a microphone/input. The AudioPipe **receiver** process plays the
received network audio into this device's output side; whatever is played to it
appears on its input (loopback), so Zoom/OBS/QuickTime/etc. hear it as a mic.

Customization is applied entirely via build-time constants (`build_driver.sh`),
so `BlackHole/BlackHole.c` stays unmodified and mergeable with upstream:

| Constant | Value |
|---|---|
| `kDriver_Name` / `kDevice_Name` | `AudioPipe` |
| `kPlugIn_BundleID` | `audio.audiopipe.AudioPipe` |
| `kNumber_Of_Channels` | `2` |
| `kSampleRates` | `48000` |
| `kLatency_Frame_Size` | `0` |

## Build

```sh
./build_driver.sh      # -> build/AudioPipe.driver  (ad-hoc signed)
```

## Install (requires sudo; restarts coreaudiod)

Installing a HAL plug-in copies the bundle into a system directory and restarts
the audio daemon (audio drops out for a second). Reversible — see uninstall.

```sh
sudo ./install.sh
```

Then check it appears:

```sh
system_profiler SPAudioDataType | grep -A2 AudioPipe   # or open "Audio MIDI Setup"
```

### If it doesn't appear
On Apple Silicon, `coreaudiod` may refuse an ad-hoc-signed plug-in depending on
your security settings. Options: sign with your Apple Developer ID
(`CODE_SIGN_IDENTITY`/`DEVELOPMENT_TEAM` in `build_driver.sh`), or check
`log show --last 2m --predicate 'process == "coreaudiod"'` for the rejection.

## Uninstall

```sh
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/AudioPipe.driver" && sudo killall coreaudiod
```

## Upstream

Forked from BlackHole `VERSION` 0.6.1. See `LICENSE` (GPL-3.0) and `CHANGELOG.md`.
