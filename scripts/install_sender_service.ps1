#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install the AudioPipe Windows sender as an auto-starting Windows Service
    (spec section 5.6) with crash auto-restart (section 10).

.DESCRIPTION
    Deploys the sender exe, soxr.dll, and config into an install directory, then
    registers a SERVICE_WIN32_OWN_PROCESS service set to start at boot (no login)
    and to auto-restart if the process dies. A Windows service runs with a clean
    environment and no user PATH, so soxr.dll MUST live next to the exe -- this
    script copies it for you.

    The service reads its config from <InstallDir>\config\sender.json. Edit that
    file (set "dest_ip" to the Mac's LAN IP) before starting.

    Run from an ELEVATED PowerShell. Requires the VC++ x64 runtime on the machine
    (already present where Visual Studio is installed).

.EXAMPLE
    .\scripts\install_sender_service.ps1 -Start
#>
param(
    [string]$ExePath     = "$PSScriptRoot\..\build\sender\audiopipe_sender.exe",
    [string]$SoxrDll     = "$env:USERPROFILE\vcpkg\installed\x64-windows\bin\soxr.dll",
    [string]$ConfigPath  = "$PSScriptRoot\..\config\sender.json",
    [string]$InstallDir  = "C:\AudioPipe",
    [string]$ServiceName = "AudioPipeSender",
    [switch]$Start
)
$ErrorActionPreference = 'Stop'

$pr = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated (Administrator) PowerShell."
}

foreach ($f in @($ExePath, $SoxrDll, $ConfigPath)) {
    if (-not (Test-Path $f)) { throw "Missing required file: $f" }
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path "$InstallDir\config" | Out-Null
Copy-Item $ExePath    "$InstallDir\audiopipe_sender.exe" -Force
Copy-Item $SoxrDll    "$InstallDir\soxr.dll"             -Force
Copy-Item $ConfigPath "$InstallDir\config\sender.json"   -Force
Write-Host "Deployed exe + soxr.dll + config to $InstallDir"

# Remove any previous instance so this is idempotent.
sc.exe query $ServiceName *> $null
if ($LASTEXITCODE -eq 0) {
    Write-Host "Removing existing service '$ServiceName'..."
    sc.exe stop $ServiceName  *> $null
    Start-Sleep -Seconds 1
    sc.exe delete $ServiceName *> $null
    Start-Sleep -Seconds 1
}

$bin = "$InstallDir\audiopipe_sender.exe"
# NOTE: sc.exe requires a space after each '='. Quote binPath in case the path has spaces.
sc.exe create $ServiceName binPath= "`"$bin`"" start= auto DisplayName= "AudioPipe Sender"
if ($LASTEXITCODE -ne 0) { throw "sc create failed (exit $LASTEXITCODE)" }
sc.exe description $ServiceName "Captures the default/virtual output via WASAPI loopback and streams it to the Mac AudioPipe receiver." *> $null

# Section 10: auto-restart if the process dies -- restart after 5s, up to 3
# times, reset the failure counter once per day.
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/5000 *> $null

Write-Host ""
Write-Host "Service '$ServiceName' installed: auto-start at boot, crash auto-restart."
Write-Host "1) Edit $InstallDir\config\sender.json  (set dest_ip to the Mac's LAN IP)"
Write-Host "2) Start it:   sc start $ServiceName"
Write-Host "   Logs go to the log_file path in that config."

if ($Start) {
    sc.exe start $ServiceName
}
