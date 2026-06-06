<#
.SYNOPSIS
    Rename the cloned Virtual-Audio-Driver's render (speaker) endpoint to
    "AudioPipe" so it shows up that way in Windows Sound settings and matches
    sender.json's device_name.

.DESCRIPTION
    Edits Source/Main/VirtualAudioDriver.inx (a UTF-16 INF template) in the
    upstream clone, renaming the speaker/device/service friendly-name strings.
    The capture (mic) endpoint is left as-is - the sender loopback-captures the
    speaker; the mic is unused. Idempotent; safe to re-run. Run this after
    cloning upstream and before building.

.EXAMPLE
    .\driver-win\apply_rename.ps1
    .\driver-win\apply_rename.ps1 -DeviceName "Studio Bus"
#>
param(
    [string]$DeviceName = "AudioPipe",
    [string]$Upstream   = "$PSScriptRoot\upstream",
    # Lowest Windows 10 build the driver installs on. Upstream targets
    # 10.0...22000 (Windows 11 only), so on Windows 10 PnP reports "no better
    # matching drivers" and binds the generic null driver. 17763 = Win10 1809.
    [string]$MinBuild   = '17763'
)
$ErrorActionPreference = 'Stop'

$inx = Join-Path $Upstream "Source\Main\VirtualAudioDriver.inx"
if (-not (Test-Path $inx)) {
    throw "INF template not found: $inx`nClone upstream first (see driver-win\README.md)."
}

$text = [System.IO.File]::ReadAllText($inx, [System.Text.Encoding]::Unicode)

# 1) Friendly name: upstream strings all read "Virtual Audio Driver by MTT"
#    (speaker szPname, topology szPname, DeviceDesc, SvcDesc). Replace the literal.
$old = 'Virtual Audio Driver by MTT'
$n1  = ([regex]::Matches($text, [regex]::Escape($old))).Count
$text = $text.Replace($old, $DeviceName)

# 2) OS floor: lower the [Manufacturer] TargetOSVersion so it installs on Win10.
$n2 = ([regex]::Matches($text, [regex]::Escape('10.0...22000'))).Count
$text = $text.Replace('10.0...22000', "10.0...$MinBuild")

# Preserve UTF-16 LE + BOM (what the WDK INF tooling expects).
[System.IO.File]::WriteAllText($inx, $text, (New-Object System.Text.UnicodeEncoding($false, $true)))
Write-Host "Renamed $n1 name string(s) to '$DeviceName'; lowered $n2 OS-floor decoration(s) to build $MinBuild."

Write-Host "Resulting INF bits:"
Get-Content $inx | Select-String 'szPname|DeviceDesc=|SvcDesc=|\.10\.0\.\.\.' | ForEach-Object { "  " + $_.Line.Trim() }
