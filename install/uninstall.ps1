# SPDX-License-Identifier: MIT
<#
  uninstall.ps1 - remove TigerLakeUsbcMux from the driver store and its devices. Run elevated (uninstall.cmd does that).
  The PMC, mux agent and IOM devices go back to having no driver, as before installation.

    .\uninstall.ps1                       remove the driver
    .\uninstall.ps1 -RemoveCertificate    also remove the test certificate that install.ps1 trusted
#>
param([switch]$RemoveCertificate)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this elevated (or use uninstall.cmd).'
}

$packages = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n" | Where-Object { $_ -match 'Original Name:\s+tigerlakeusbcmux\.inf' }
if (-not $packages) { Write-Host 'TigerLakeUsbcMux is not installed.' }
foreach ($p in $packages) {
  $name = ([regex]'Published Name:\s+(\S+)').Match($p).Groups[1].Value
  pnputil /delete-driver $name /uninstall /force | Select-String 'deleted|Failed|uninstalled' | ForEach-Object { "${name}: $($_.Line.Trim())" }
}

if ($RemoveCertificate) {
  $cer = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (Join-Path $here 'TigerLakeUsbcMuxTest.cer')
  foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $cer.Thumbprint | Remove-Item
  }
  Write-Host "Removed certificate $($cer.Thumbprint)."
}
Write-Host 'Restart Windows to finish removing the driver.'
