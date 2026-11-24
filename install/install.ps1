# SPDX-License-Identifier: MIT
<#
  install.ps1 - install the test-signed TigerLakeUsbcMux driver. Run elevated from the folder that holds
  TigerLakeUsbcMux.sys/.inf/.cat and TigerLakeUsbcMuxTest.cer (install.cmd does both for you).

  Requires test signing (bcdedit /set testsigning on, which needs Secure Boot off) and CoolStar's
  Chromebook EC bus driver (crosecbus). Older TigerLakeUsbcMux packages are removed afterwards.

    .\install.ps1          install or update
    .\install.ps1 -Force   skip the supported-hardware check
#>
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this elevated (or use install.cmd).'
}

if (-not ((bcdedit /enum '{current}' | Out-String) -match 'testsigning\s+Yes')) {
  throw "Test signing is off. Disable Secure Boot in the firmware, run 'bcdedit /set testsigning on' elevated, restart, then run this again."
}

$devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue
if (-not $Force -and -not ($devices | Where-Object { $_.InstanceId -like 'ACPI\INTC1026\*' })) {
  throw 'No Tiger Lake / Alder Lake PMC (ACPI\INTC1026) found; this machine does not look supported. Use -Force to install anyway.'
}
$bus = $devices | Where-Object { $_.InstanceId -like 'ACPI\GOOG0004\*' } | Select-Object -First 1
if (-not $bus -or $bus.Status -ne 'OK') {
  Write-Warning 'The ChromeOS EC bus driver (CoolStar crosecbus, ACPI\GOOG0004) is missing or not working. TigerLakeUsbcMux needs it to talk to the EC; install it first.'
}

# The release is test-signed with this certificate, so Windows must trust it.
$cer = Join-Path $here 'TigerLakeUsbcMuxTest.cer'
foreach ($store in 'Root', 'TrustedPublisher') {
  Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}

$result = pnputil /add-driver (Join-Path $here 'TigerLakeUsbcMux.inf') /install | Out-String
$pnpExit = $LASTEXITCODE
$result
if ($pnpExit -ne 0 -and $pnpExit -ne 3010) { throw "pnputil failed ($pnpExit)" }
$published = ([regex]'Published Name:\s+(\S+)').Match($result).Groups[1].Value
Start-Sleep -Seconds 2

# Drop superseded copies of the package (pnputil refuses to delete one still in use).
$packages = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n" | Where-Object { $_ -match 'Original Name:\s+tigerlakeusbcmux\.inf' }
foreach ($p in $packages) {
  $name = ([regex]'Published Name:\s+(\S+)').Match($p).Groups[1].Value
  if ($name -and $name -ne $published) {
    pnputil /delete-driver $name | Select-String 'deleted|Failed' | ForEach-Object { "${name}: $($_.Line.Trim())" }
  }
}

# List whichever supported ACPI devices are present (INTC105C may not be enumerated on all platforms).
$matched = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
  Where-Object { $_.InstanceId -match '^ACPI\\(INTC1026|INTC105C|INTC1072|INTC1079)\\' }
$matched | ForEach-Object {
  '{0,-8} {1,-46} {2}' -f $_.Status, $_.FriendlyName, $_.Problem
}
if (-not ($matched | Where-Object { $_.InstanceId -match '^ACPI\\(INTC1072|INTC1079)\\' })) {
  Write-Warning 'No supported Type-C IO Manager (ACPI\INTC1072 or ACPI\INTC1079) was found.'
}
if ($pnpExit -eq 3010 -or $result -match 'reboot') { Write-Host 'Restart Windows to finish the installation.' }
