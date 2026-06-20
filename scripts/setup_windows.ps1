#Requires -RunAsAdministrator
<#
.SYNOPSIS
    AudioPipe -- one-shot Windows build + install (the SEND side).

.DESCRIPTION
    Does the whole Windows setup in one elevated command by calling the
    component scripts:
      1. build  -- cmake configure + build the sender (Release)
      2. driver -- build the "AudioPipe" virtual OUTPUT device (clone upstream +
                   apply_rename + apply_48k + build.ps1; needs the WDK)
      3. install-- persistent device + auto-start sender Service (install_all.ps1)

    Test-signing: the driver's test-signed .sys only loads in Test Mode, which
    needs ONE reboot. If test-signing is off, this turns it on and asks you to
    reboot, then re-run to finish. Re-run safe.

    Run from an ELEVATED PowerShell (Win+X -> Terminal (Admin)). If scripts are
    blocked: Set-ExecutionPolicy -Scope Process Bypass -Force

    The macOS half is set up by scripts/setup_macos.sh.

.EXAMPLE
    .\scripts\setup_windows.ps1 -Start
.EXAMPLE
    .\scripts\setup_windows.ps1 -SkipBuild -Start      # after the test-signing reboot
.EXAMPLE
    .\scripts\setup_windows.ps1 -SkipDriver -Start      # device already installed; sender only
.EXAMPLE
    .\scripts\setup_windows.ps1 -Uninstall
#>
param(
    [switch]$SkipBuild,    # skip the cmake sender build
    [switch]$SkipDriver,   # skip building + installing the virtual output device
    [switch]$Start,        # start the service when done
    [switch]$Uninstall     # remove the service (prints how to remove the driver)
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if ($Uninstall) {
    & (Join-Path $root 'scripts\uninstall_sender_service.ps1') -Purge
    Write-Host ""
    Write-Host "Service removed. To remove the driver too:"
    Write-Host "  pnputil /enum-drivers                         (find the oemNN.inf for AudioPipe)"
    Write-Host "  pnputil /delete-driver oemNN.inf /uninstall   (elevated)"
    Write-Host "  bcdedit /set testsigning off                  (optional; reboot to apply)"
    return
}

# ---- [1/3] build the sender -------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "==== [1/3] Building the sender (cmake, Release) ===="
    & cmake -S $root -B (Join-Path $root 'build')
    if ($LASTEXITCODE) { throw "cmake configure failed (is CMake on PATH?)" }
    & cmake --build (Join-Path $root 'build') --config Release
    if ($LASTEXITCODE) { throw "cmake build failed" }
} else {
    Write-Host "[1/3] Skipping sender build (-SkipBuild)."
}

# ---- [2/3] build the virtual output driver ---------------------------------
if (-not $SkipDriver) {
    Write-Host "==== [2/3] Building the AudioPipe virtual output driver ===="
    $dw       = Join-Path $root 'driver-win'
    $upstream = Join-Path $dw 'upstream'
    if (-not (Test-Path (Join-Path $upstream '.git'))) {
        Write-Host "Cloning Virtual-Audio-Driver upstream..."
        & git clone https://github.com/VirtualDrivers/Virtual-Audio-Driver.git $upstream
        if ($LASTEXITCODE) { throw "git clone failed (is git on PATH?)" }
    }
    & (Join-Path $dw 'apply_rename.ps1')   # rename endpoint -> AudioPipe, lower OS floor
    & (Join-Path $dw 'apply_48k.ps1')      # pin the endpoint to 48 kHz
    & (Join-Path $dw 'build.ps1')          # Release/x64, test-signs the .sys (needs the WDK)
} else {
    Write-Host "[2/3] Skipping driver build (-SkipDriver)."
}

# ---- [3/3] test-signing gate + install -------------------------------------
Write-Host "==== [3/3] Install ===="
$testsigning = ((& bcdedit) -join "`n") -match 'testsigning\s+Yes'
if (-not $testsigning) {
    Write-Host "Test-signing is OFF -- the test-signed driver .sys cannot load without it."
    & bcdedit /set testsigning on | Out-Null
    $rerun = ".\scripts\setup_windows.ps1 -SkipBuild"
    if ($SkipDriver) { $rerun += " -SkipDriver" }
    if ($Start)      { $rerun += " -Start" }
    Write-Host ""
    Write-Host "Enabled test-signing. REBOOT, then re-run (elevated) to finish:"
    Write-Host "  $rerun"
    return
}

$installArgs = @{}
if ($Start)      { $installArgs['Start']      = $true }
if ($SkipDriver) { $installArgs['SkipDriver'] = $true }
& (Join-Path $root 'scripts\install_all.ps1') @installArgs

Write-Host ""
Write-Host "==== Windows setup done ===="
Write-Host "Edit C:\AudioPipe\config\sender.json:  dest_ip = the Mac's IP,  device_name = 'AudioPipe'"
Write-Host "Route a specific app to the 'AudioPipe' output in Settings > Sound > Volume mixer."
Write-Host "Uninstall: .\scripts\setup_windows.ps1 -Uninstall"
