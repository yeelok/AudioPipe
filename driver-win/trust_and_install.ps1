#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Trust the driver's test-signing certificate and install the AudioPipe
    virtual audio device.

.DESCRIPTION
    A Release build test-signs VirtualAudioDriver.sys / .cat with an
    auto-generated WDK test certificate. For Windows to load and install that
    driver you must (1) enable test-signing and reboot, and (2) trust the signer
    certificate. This script extracts the signer cert from the built .sys, adds
    it to LocalMachine Root + TrustedPublisher, then installs the driver with
    pnputil.

    Run from an ELEVATED PowerShell, AFTER:
        bcdedit /set testsigning on   &&   reboot

.EXAMPLE
    .\driver-win\trust_and_install.ps1
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

# Warn (don't hard-fail) if test-signing is off: the install may appear to work
# but the .sys won't load until test-signing is on and the machine rebooted.
$tsOn = bcdedit /enum '{current}' 2>$null | Select-String 'testsigning\s+Yes'
if (-not $tsOn) {
    Write-Warning "Test signing looks OFF. Run:  bcdedit /set testsigning on   then REBOOT, then re-run this."
}

# Trust the cert the build used to test-sign the driver.
$sig = Get-AuthenticodeSignature $sys
if (-not $sig.SignerCertificate) {
    throw "$sys is not Authenticode-signed. A WDK Release build should test-sign it; rebuild with the WDK installed (or sign manually - see README)."
}
$cert = $sig.SignerCertificate
$cer  = Join-Path $env:TEMP "audiopipe_driver.cer"
[System.IO.File]::WriteAllBytes($cer, $cert.Export('Cert'))
certutil -addstore -f Root            "$cer" | Out-Null
certutil -addstore -f TrustedPublisher "$cer" | Out-Null
Remove-Item "$cer" -ErrorAction SilentlyContinue
Write-Host "Trusted signer cert: $($cert.Subject)  [$($cert.Thumbprint)]"

# Add the driver package to the store (and install on any matching device).
pnputil /add-driver "$inf" /install

# This is a ROOT-enumerated software device: pnputil publishes the package but
# does not create a device node, so nothing appears until we create one. devgen
# (WDK Tools) creates a software device with our hardware id; PnP then binds the
# just-installed driver to it. Idempotent via a fixed instance id.
$hwid = 'root\VirtualAudioDriver'
$existing = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VirtualAudioDriver' }
if ($existing) {
    Write-Host "Device node already present: $(($existing.InstanceId) -join ', ')"
} else {
    $devgen = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Tools" -Filter devgen.exe -Recurse -ErrorAction SilentlyContinue |
              Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $devgen) {
        Write-Warning "devgen.exe not found (WDK Tools). Create the device manually: Device Manager > Action > Add legacy hardware > install from $inf."
    } else {
        & $devgen.FullName /add /instanceid AudioPipe /hardwareid $hwid
        Write-Host "Created software device ($hwid)."
    }
}

Write-Host ""
Write-Host "Done. Verify: Settings > System > Sound should list 'AudioPipe' as an output."
Write-Host "(Requires test-signing ON + a prior reboot for the .sys to load.)"
Write-Host "Then set config\sender.json device_name to 'AudioPipe' and (re)start the sender."
