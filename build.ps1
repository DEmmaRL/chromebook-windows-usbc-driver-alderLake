# SPDX-License-Identifier: MIT
<#
  build.ps1 - build, catalog and test-sign TigerLakeUsbcMux and usbcctl without the VS WDK integration.

  Needs Visual Studio 2022 (or Build Tools) with the x64 C++ tools, and the WDK.
  Output goes to out\: TigerLakeUsbcMux.sys, TigerLakeUsbcMux.inf, TigerLakeUsbcMux.cat, usbcctl.exe, TigerLakeUsbcMuxTest.cer.

    .\build.ps1                 build and test-sign
    .\build.ps1 -NoSign         build only
#>
param(
  [switch]$NoSign,
  [string]$KmdfVersion = '1.15'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'out'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $out, $obj | Out-Null

# --- toolchain -------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ x64 tools not found' }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}

$kit = "${env:ProgramFiles(x86)}\Windows Kits\10"
$wdkVer = (Get-ChildItem "$kit\Include" -Directory | Where-Object { Test-Path "$($_.FullName)\km\wdm.h" } |
           Sort-Object { [version]$_.Name } | Select-Object -Last 1).Name
if (-not $wdkVer) { throw 'WDK kernel headers not found' }
$wdkBin = Get-ChildItem "$kit\bin" -Directory | Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending
function Find-KitTool($name) {
  foreach ($d in $wdkBin) { foreach ($a in 'x64', 'x86') { $p = Join-Path $d.FullName "$a\$name"; if (Test-Path $p) { return $p } } }
  throw "$name not found in $kit\bin"
}
Write-Host "WDK $wdkVer, KMDF $KmdfVersion"

function Invoke-Tool($exe, [string[]]$arguments) {
  & $exe @arguments | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "$exe failed ($LASTEXITCODE)" }
}

# --- version resource --------------------------------------------------------
# Version from DriverVer in the INF, copyright from LICENSE (if present), so each is set in one place.
$driverVer = Select-String -Path "$root\driver\TigerLakeUsbcMux.inf" -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\d+)\.(\d+)\.(\d+)\.(\d+)'
if (-not $driverVer) { throw 'DriverVer (a.b.c.d) not found in TigerLakeUsbcMux.inf' }
$ver = $driverVer.Matches[0].Groups[1..4] | ForEach-Object Value
$copyright = 'MIT License'
if (Test-Path "$root\LICENSE") {
  $line = Select-String -Path "$root\LICENSE" -Pattern '^Copyright' | Select-Object -First 1
  if ($line) { $copyright = $line.Line.Trim() }
}
function ConvertTo-RcString($s) { '"' + ($s -replace '\\', '\\' -replace '"', '""') + '"' }
@(
  "#define VER_MAJOR $($ver[0])", "#define VER_MINOR $($ver[1])",
  "#define VER_BUILD $($ver[2])", "#define VER_REVISION $($ver[3])",
  "#define VER_STRING $(ConvertTo-RcString ($ver -join '.'))",
  "#define VER_COMPANY $(ConvertTo-RcString 'Open Source Driver')",
  "#define VER_PRODUCT $(ConvertTo-RcString 'Tiger Lake Chromebook USB-C Driver')",
  "#define VER_COPYRIGHT $(ConvertTo-RcString $copyright)"
) | Set-Content "$obj\version.h" -Encoding ascii
$rc = Find-KitTool 'rc.exe'
Write-Host "version $($ver -join '.')"

# --- driver ----------------------------------------------------------------
Invoke-Tool $rc @('/nologo', "/I$obj", "/fo$obj\TigerLakeUsbcMux.res", "$root\driver\TigerLakeUsbcMux.rc")
$kmdfMinor = $KmdfVersion.Split('.')[1]
$cflags = @(
  '/nologo', '/c', '/kernel', '/W4', '/WX', '/Zi', '/O2', '/Oi', '/GS', '/Gy', '/GF', '/Zp8', '/std:c17',
  '/wd4324', '/D_AMD64_', '/DAMD64', '/D_WIN64','/DNTDDI_VERSION=0x0A000008', '/D_WIN32_WINNT=0x0A00',
  '/DWINVER=0x0A00', '/DKMDF_VERSION_MAJOR=1', "/DKMDF_VERSION_MINOR=$kmdfMinor",
  # /X drops the user-mode INCLUDE path set by vcvars: kernel CRT + WDK headers first, compiler intrinsics last.
  # The WDK ships only part of shared\; the rest comes from the SDK that vcvars selected.
  '/X', "/I$kit\Include\$wdkVer\km\crt", "/I$kit\Include\$wdkVer\km", "/I$kit\Include\$wdkVer\shared",
  "/I$kit\Include\$($env:WindowsSDKVersion.TrimEnd('\'))\shared",
  "/I$kit\Include\wdf\kmdf\$KmdfVersion", "/I$env:VCToolsInstallDir\include",
  "/Fo$obj\", "/Fd$obj\TigerLakeUsbcMux.pdb"
)
$sources = Get-ChildItem "$root\driver\*.c" | ForEach-Object FullName
Invoke-Tool cl.exe ($cflags + $sources)

$km = "$kit\Lib\$wdkVer\km\x64"
$wdf = "$kit\Lib\wdf\kmdf\x64\$KmdfVersion"
$lflags = @(
  '/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,10.00', '/ENTRY:FxDriverEntry', '/NODEFAULTLIB', '/MACHINE:X64',
  '/OPT:REF', '/OPT:ICF', '/RELEASE', '/DEBUG', '/INCREMENTAL:NO', '/NXCOMPAT', '/DYNAMICBASE',
  '/MERGE:_TEXT=.text;_PAGE=PAGE', '/SECTION:INIT,d',
  # /PDBALTPATH: the driver records only "TigerLakeUsbcMux.pdb", not the build folder (which contains the user name).
  "/OUT:$out\TigerLakeUsbcMux.sys", "/PDB:$out\TigerLakeUsbcMux.pdb", '/PDBALTPATH:%_PDB%',
  "$km\ntoskrnl.lib", "$km\hal.lib", "$km\BufferOverflowFastFailK.lib",
  "$wdf\WdfLdr.lib", "$wdf\WdfDriverEntry.lib", "$obj\TigerLakeUsbcMux.res"
)
Invoke-Tool link.exe ($lflags + (Get-ChildItem "$obj\*.obj" | Where-Object Name -ne 'usbcctl.obj' | ForEach-Object FullName))
Copy-Item "$root\driver\TigerLakeUsbcMux.inf" $out -Force

# --- usbcctl --------------------------------------------------------------
Invoke-Tool $rc @('/nologo', "/I$obj", "/fo$obj\usbcctl.res", "$root\tools\usbcctl\usbcctl.rc")
Invoke-Tool cl.exe @('/nologo', '/W4', '/WX', '/O2', '/std:c17', "/Fo$obj\", "/Fe$out\usbcctl.exe",
                     "$root\tools\usbcctl\usbcctl.c", "$obj\usbcctl.res", 'setupapi.lib')

# --- catalog + test signing ------------------------------------------------
if ($NoSign) { return }

$subject = 'CN=Open Source Driver'
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $subject | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My `
          -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $cert -FilePath "$out\TigerLakeUsbcMuxTest.cer" | Out-Null

$signtool = Find-KitTool 'signtool.exe'
Invoke-Tool $signtool @('sign', '/q', '/fd', 'sha256', '/sha1', $cert.Thumbprint, "$out\TigerLakeUsbcMux.sys")
Remove-Item "$out\TigerLakeUsbcMux.cat" -ErrorAction SilentlyContinue
$staging = Join-Path $obj 'package'
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $staging | Out-Null
Copy-Item "$out\TigerLakeUsbcMux.sys", "$out\TigerLakeUsbcMux.inf" $staging
Invoke-Tool (Find-KitTool 'inf2cat.exe') @("/driver:$staging", '/os:10_VB_X64', '/uselocaltime')
Move-Item "$staging\TigerLakeUsbcMux.cat" $out -Force
Invoke-Tool $signtool @('sign', '/q', '/fd', 'sha256', '/sha1', $cert.Thumbprint, "$out\TigerLakeUsbcMux.cat")
Write-Host "signed with $($cert.Thumbprint)"
