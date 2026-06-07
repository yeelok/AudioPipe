#Requires -RunAsAdministrator
<#
.SYNOPSIS
    One-shot installer for the whole AudioPipe Windows side:
      1) the "AudioPipe" virtual OUTPUT device (test-signed driver, PERSISTENT root node)
      2) the sender as an auto-start, crash-restarting Windows Service

.DESCRIPTION
    Thin orchestrator: runs the existing component scripts in order, so the same
    logic stays in one place each:
      driver-win\install_persistent.ps1   - trust cert + add driver + devcon root device
      scripts\install_sender_service.ps1  - deploy exe+config to the install dir + register service

    THIS SCRIPT ONLY INSTALLS - build the artifacts first:
      Driver:  cd driver-win;  .\build.ps1                       (needs the WDK)
      Sender:  cmake -S . -B build;  cmake --build build --config Release

    Requirements:
      - ELEVATED PowerShell.
      - Test-signing ON (Test Mode watermark) so the driver .sys loads. If it is off,
        install_persistent.ps1 warns; run 'bcdedit /set testsigning on', reboot, re-run.

.EXAMPLE
    .\scripts\install_all.ps1 -Start
.EXAMPLE
    .\scripts\install_all.ps1 -SkipDriver -Start      # device already present; (re)install only the service
#>
param(
    [string]$InstallDir  = 'C:\AudioPipe',
    [string]$ServiceName = 'AudioPipeSender',
    [switch]$Start,         # start the service when done
    [switch]$SkipDriver     # install only the sender service (skip the device)
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$pr = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated (Administrator) PowerShell."
}

# Fail fast if a build artifact is missing, so we never leave a half install.
$senderExe = Join-Path $root 'build\sender\audiopipe_sender.exe'
$driverInf = Join-Path $root 'driver-win\upstream\x64\Release\package\VirtualAudioDriver.inf'
if (-not (Test-Path $senderExe)) {
    throw "Sender not built: $senderExe`n  Build (repo root): cmake -S . -B build; cmake --build build --config Release"
}
if (-not $SkipDriver -and -not (Test-Path $driverInf)) {
    throw "Driver not built: $driverInf`n  Build: cd driver-win; .\build.ps1   (needs the WDK)"
}

Write-Host "==== AudioPipe Windows install ===="

# ---- [1/2] virtual output device (persistent root node) ----
if ($SkipDriver) {
    Write-Host "[1/2] Skipping driver/device (-SkipDriver)."
} else {
    Write-Host "[1/2] Installing the AudioPipe virtual output device (persistent)..."
    & (Join-Path $root 'driver-win\install_persistent.ps1')
}

# ---- [2/2] sender Windows Service ----
Write-Host ""
Write-Host "[2/2] Installing the sender Windows Service..."
$svc = @{ InstallDir = $InstallDir; ServiceName = $ServiceName }
if ($Start) { $svc['Start'] = $true }
& (Join-Path $root 'scripts\install_sender_service.ps1') @svc

Write-Host ""
Write-Host "==== Done ===="
Write-Host "Device:  'AudioPipe' output - route an app to it in Settings > Sound > Volume mixer"
Write-Host "Service: $ServiceName - auto-start at boot, crash-restart"
Write-Host "Config:  $InstallDir\config\sender.json  (dest_ip = Mac IP, device_name = 'AudioPipe')"
if (-not $Start) { Write-Host "Start:   sc start $ServiceName" }
Write-Host ""
Write-Host "CAVEAT: the service runs as LocalSystem in session 0. WASAPI loopback may not"
Write-Host "reach session-0 audio on every setup. If the Mac gets no audio, first verify in"
Write-Host "your user session:  .\build\sender\audiopipe_sender.exe --console --config $InstallDir\config\sender.json"
