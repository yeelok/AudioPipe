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
    [string]$Upstream = "$PSScriptRoot\upstream"
)
$ErrorActionPreference = 'Stop'

$sln = Join-Path $Upstream "VirtualAudioDriver.sln"
if (-not (Test-Path $sln)) {
    throw "Solution not found: $sln`nClone upstream first (see driver-win\README.md)."
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe |
           Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found via vswhere. Install VS + the WDK." }
Write-Host "MSBuild: $msbuild"

& $msbuild $sln /p:Configuration=$Config /p:Platform=$Platform /m /nologo
if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE). Most often this means the WDK isn't installed/matched to your SDK."
}

$pkg = Join-Path $Upstream "$Platform\$Config\package"
Write-Host ""
Write-Host "Build OK. Package dir: $pkg"
Get-ChildItem $pkg -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.sys','.inf','.cat' } | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "Next: enable test-signing + reboot, then run trust_and_install.ps1 (elevated)."
