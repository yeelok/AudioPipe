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
    [string]$Upstream   = "$PSScriptRoot\upstream"
)
$ErrorActionPreference = 'Stop'

$inx = Join-Path $Upstream "Source\Main\VirtualAudioDriver.inx"
if (-not (Test-Path $inx)) {
    throw "INF template not found: $inx`nClone upstream first (see driver-win\README.md)."
}

# The upstream strings all read "Virtual Audio Driver by MTT" (speaker szPname,
# topology szPname, DeviceDesc, SvcDesc). Replace that exact literal.
$old  = 'Virtual Audio Driver by MTT'
$text = [System.IO.File]::ReadAllText($inx, [System.Text.Encoding]::Unicode)
$n    = ([regex]::Matches($text, [regex]::Escape($old))).Count

if ($n -gt 0) {
    $text = $text.Replace($old, $DeviceName)
    # Preserve UTF-16 LE + BOM (what the WDK INF tooling expects).
    [System.IO.File]::WriteAllText($inx, $text, (New-Object System.Text.UnicodeEncoding($false, $true)))
    Write-Host "Renamed $n string(s) to '$DeviceName' in VirtualAudioDriver.inx"
} else {
    Write-Host "No '$old' strings found (already renamed?) - leaving the file unchanged."
}

Write-Host "Resulting friendly names:"
Get-Content $inx | Select-String 'szPname|DeviceDesc=|SvcDesc=' | ForEach-Object { "  " + $_.Line.Trim() }
