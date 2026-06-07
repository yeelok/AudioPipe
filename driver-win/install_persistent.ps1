#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install the AudioPipe virtual audio device as a PERSISTENT root-enumerated
    device that survives reboots.

.DESCRIPTION
    trust_and_install.ps1 creates the device with devgen; devgen (SwDeviceCreate)
    software devices are boot-scoped and are removed on reboot, leaving a
    CM_PROB_PHANTOM remnant so "AudioPipe" disappears from Sound. This script
    instead creates a ROOT-enumerated devnode with devcon.exe, which persists
    across reboots (as long as test-signing stays on).

    It trusts the build's test-signing cert, adds the driver package to the
    store, removes any prior AudioPipe nodes (the devgen phantom + earlier root
    installs), then runs `devcon install` for hardware id root\VirtualAudioDriver.

    Run from an ELEVATED PowerShell. Requires test-signing ON (Test Mode
    watermark) and the WDK (provides devcon.exe).

.EXAMPLE
    .\driver-win\install_persistent.ps1
#>
param(
    [string]$Upstream = "$PSScriptRoot\upstream",
    [ValidateSet('Release','Debug')] [string]$Config   = 'Release',
    [ValidateSet('x64','ARM64')]     [string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'

$pr = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated (Administrator) PowerShell."
}

$pkg = Join-Path $Upstream "$Platform\$Config\package"
$inf = Join-Path $pkg "VirtualAudioDriver.inf"
$sys = Join-Path $pkg "VirtualAudioDriver.sys"
foreach ($f in @($inf, $sys)) {
    if (-not (Test-Path $f)) { throw "Missing $f - build first: .\driver-win\build.ps1" }
}

# Test-signing must be ON or the .sys won't load (device would be present-but-failed).
$tsOn = bcdedit /enum '{current}' 2>$null | Select-String 'testsigning\s+Yes'
if (-not $tsOn) {
    Write-Warning "Test-signing looks OFF. Run:  bcdedit /set testsigning on  then REBOOT, then re-run this."
}

# 1. Trust the cert the build used to test-sign the driver (idempotent).
$sig = Get-AuthenticodeSignature $sys
if (-not $sig.SignerCertificate) { throw "$sys is not Authenticode-signed; rebuild with the WDK (see README)." }
$cert = $sig.SignerCertificate
$cer  = Join-Path $env:TEMP "audiopipe_driver.cer"
[System.IO.File]::WriteAllBytes($cer, $cert.Export('Cert'))
certutil -addstore -f Root             "$cer" | Out-Null
certutil -addstore -f TrustedPublisher "$cer" | Out-Null
Remove-Item "$cer" -ErrorAction SilentlyContinue
Write-Host "Trusted signer cert: $($cert.Subject)"

# 2. Add the driver package to the driver store (idempotent).
pnputil /add-driver "$inf" /install | Out-Null

# 3. Locate devcon.exe (WDK Tools).
$devcon = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Tools" -Filter devcon.exe -Recurse -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $devcon) {
    throw "devcon.exe not found (WDK Tools). Use Device Manager > Add legacy hardware > Have Disk > $inf instead."
}
$hwid = 'root\VirtualAudioDriver'

# 4. Remove prior nodes so we don't stack duplicates:
#    the non-persistent devgen phantom, and any earlier root install of this hwid.
if (Get-PnpDevice -InstanceId 'SWD\DEVGEN\AUDIOPIPE' -ErrorAction SilentlyContinue) {
    Write-Host "Removing the non-persistent devgen node (SWD\DEVGEN\AUDIOPIPE)..."
    pnputil /remove-device 'SWD\DEVGEN\AUDIOPIPE' | Out-Null
}
$ErrorActionPreference = 'Continue'      # 'remove' is harmless if nothing matches
& $devcon.FullName remove $hwid | Out-Null
$ErrorActionPreference = 'Stop'

# 5. Create a PERSISTENT root-enumerated devnode; PnP binds the installed driver.
Write-Host "Creating persistent root device ($hwid)..."
& $devcon.FullName install "$inf" $hwid
Start-Sleep -Seconds 2

# 6. Verify it bound our driver (oemNN.inf), not the generic null driver.
$node = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -eq 'AudioPipe' -or $_.InstanceId -match 'VIRTUALAUDIODRIVER' } |
        Select-Object -First 1
if ($node) {
    $binf = ($node | Get-PnpDeviceProperty DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
    Write-Host ("Device: {0}  status={1}  driver={2}" -f $node.InstanceId, $node.Status, $binf)
    if ($binf -eq 'c_swdevice.inf') {
        Write-Warning "Bound the generic driver - check the INF [Manufacturer] OS build floor (see README)."
    } else {
        Write-Host "OK: AudioPipe installed as a persistent root device (survives reboots while test-signing stays on)."
    }
} else {
    Write-Warning "No present AudioPipe node found after install - check Device Manager / C:\Windows\INF\setupapi.dev.log."
}

Write-Host ""
Write-Host "Verify now: Settings > System > Sound lists 'AudioPipe' as an output."
Write-Host "Then reboot once and confirm it is STILL there (that proves persistence)."
Write-Host "Finally set config\sender.json device_name to 'AudioPipe'."
