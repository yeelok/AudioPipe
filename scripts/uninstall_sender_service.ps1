#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Stop and remove the AudioPipe sender Windows Service.

.DESCRIPTION
    Stops and deletes the service. Pass -Purge to also delete the install
    directory (exe, config, logs). Run from an elevated PowerShell.

.EXAMPLE
    .\scripts\uninstall_sender_service.ps1 -Purge
#>
param(
    [string]$ServiceName = "AudioPipeSender",
    [string]$InstallDir  = "C:\AudioPipe",
    [switch]$Purge
)
$ErrorActionPreference = 'Stop'

$pr = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated (Administrator) PowerShell."
}

sc.exe query $ServiceName *> $null
if ($LASTEXITCODE -eq 0) {
    sc.exe stop $ServiceName *> $null
    Start-Sleep -Seconds 1
    sc.exe delete $ServiceName
    Write-Host "Service '$ServiceName' removed."
} else {
    Write-Host "Service '$ServiceName' not installed."
}

if ($Purge -and (Test-Path $InstallDir)) {
    Remove-Item -Recurse -Force $InstallDir
    Write-Host "Deleted $InstallDir"
}
