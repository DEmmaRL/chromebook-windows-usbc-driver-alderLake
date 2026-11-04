# SPDX-License-Identifier: MIT
<#
  package.ps1 - build, test-sign and zip a release for GitHub.

  Output in dist\:
    TigerLakeUsbcMux-<version>-x64.zip   the driver (TigerLakeUsbcMux.sys/.inf/.cat), its test certificate, the
                                         install and uninstall scripts, and the license files that must accompany
                                         binary copies
    SHA256SUMS.txt                       checksum of the zip
  The version is the DriverVer in driver\TigerLakeUsbcMux.inf. usbcctl.exe and TigerLakeUsbcMux.pdb stay in out\ (developer use).

    .\package.ps1             build with build.ps1, then package
    .\package.ps1 -NoBuild    package the existing out\ build
#>
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'out'
$dist = Join-Path $root 'dist'

if (-not $NoBuild) { & (Join-Path $root 'build.ps1') }

$inf = Get-Content (Join-Path $root 'driver\TigerLakeUsbcMux.inf') | Select-String '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)'
if (-not $inf) { throw 'DriverVer not found in TigerLakeUsbcMux.inf' }
$version = $inf.Matches[0].Groups[1].Value
$name = "TigerLakeUsbcMux-$version-x64"

$driverFiles = 'TigerLakeUsbcMux.sys', 'TigerLakeUsbcMux.inf', 'TigerLakeUsbcMux.cat', 'TigerLakeUsbcMuxTest.cer'
foreach ($f in $driverFiles) {
  if (-not (Test-Path (Join-Path $out $f))) { throw "out\$f missing - run build.ps1 (with signing) first" }
}

$stage = Join-Path $dist $name
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item ($driverFiles | ForEach-Object { Join-Path $out $_ }) $stage
Copy-Item (Join-Path $root 'install\*') $stage
Copy-Item (Join-Path $root 'LICENSE'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') $stage
Copy-Item (Join-Path $root 'licenses') $stage -Recurse

$zip = Join-Path $dist "$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item $stage -Recurse -Force

# LF line ending, so that sha256sum -c can read the file.
$sum = Get-FileHash $zip -Algorithm SHA256
[IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS.txt'),
  ('{0}  {1}' -f $sum.Hash.ToLower(), (Split-Path $zip -Leaf)) + "`n")
Get-Content (Join-Path $dist 'SHA256SUMS.txt')
