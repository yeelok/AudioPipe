<#
.SYNOPSIS
    Build the (renamed) Virtual-Audio-Driver fork - Release x64 by default.

.DESCRIPTION
    Locates MSBuild via vswhere and builds VirtualAudioDriver.sln. Requires
    Visual Studio + the Windows Driver Kit (WDK) matching your SDK; without the
    WDK the driver build targets are missing and the build fails. Output lands in
    upstream\<Platform>\<Config>\package\ (VirtualAudioDriver.sys/.inf/.cat).

    A normal Release build test-signs the .sys and generates+signs the .cat with
    an auto-created WDK test certificate - trust_and_install.ps1 then trusts that
    cert and installs the driver.

.EXAMPLE
    .\driver-win\build.ps1
#>
param(
    [ValidateSet('Release','Debug')] [string]$Config   = 'Release',
    [ValidateSet('x64','ARM64')]     [string]$Platform = 'x64',
    [string]$Upstream = "$PSScriptRoot\upstream",
    # DriverVer date stamped into the INF. inf2cat rejects a date in the future,
    # and StampInf uses the LOCAL date by default while inf2cat validates against
    # UTC -- so a machine ahead of UTC stamps "tomorrow" and the catalog build
    # fails. Pin a safely-past date to avoid that. Override if you like.
    [string]$DriverVerDate = '01/01/2025'
)
$ErrorActionPreference = 'Stop'

$sln = Join-Path $Upstream "VirtualAudioDriver.sln"
if (-not (Test-Path $sln)) {
    throw "Solution not found: $sln`nClone upstream first (see driver-win\README.md)."
}

# Drop-in props that apply to every project in the upstream tree:
#  - Pin the INF DriverVer date (fixes inf2cat "postdated DriverVer" on machines
#    whose local date is ahead of UTC).
# Paired with the msbuild flags below, this skips the WDK's universal-driver
# validators (InfVerif / ApiValidator), which aren't needed for a locally
# test-signed personal driver and are broken/missing in some preview WDKs.
$props = Join-Path $Upstream "Directory.Build.props"
@"
<Project>
  <ItemDefinitionGroup>
    <Inf>
      <DateStamp>$DriverVerDate</DateStamp>
    </Inf>
  </ItemDefinitionGroup>
</Project>
"@ | Set-Content -Path $props -Encoding UTF8
Write-Host "Wrote $props (DriverVer date $DriverVerDate)"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe |
           Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found via vswhere. Install VS + the WDK." }
Write-Host "MSBuild: $msbuild"

& $msbuild $sln /p:Configuration=$Config /p:Platform=$Platform /m /nologo `
    /p:SkipPackageVerification=true /p:ApiValidator_Enable=false /p:RunCodeAnalysis=false
if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE). If it's the WDK validators (InfVerif/ApiValidator) or a 'postdated DriverVer', see driver-win\README.md; otherwise the WDK likely isn't matched to your SDK."
}

$pkg = Join-Path $Upstream "$Platform\$Config\package"
Write-Host ""
Write-Host "Build OK. Package dir: $pkg"
Get-ChildItem $pkg -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.sys','.inf','.cat' } | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "Next: enable test-signing + reboot, then run trust_and_install.ps1 (elevated)."
