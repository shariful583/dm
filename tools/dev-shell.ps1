# SPDX-License-Identifier: GPL-3.0-or-later
#
# Loads the MSVC x64 developer environment (cl, link, cmake, ninja) into the current PowerShell
# session. Dot-source it:   . .\tools\dev-shell.ps1
# Uses the newest Visual Studio with the C++ toolset. If VCPKG_ROOT is not set, falls back to the
# vcpkg bundled with Visual Studio.

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio with C++ tools.' }

$vsPath = & $vswhere -latest -prerelease -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'No Visual Studio with the MSVC x64 toolset found.' }

# VsDevCmd calls vswhere by name; make it resolvable.
$env:PATH = (Split-Path $vswhere) + ';' + $env:PATH

Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

if (-not $env:VCPKG_ROOT) {
    $bundled = Join-Path $vsPath 'VC\vcpkg'
    if (Test-Path (Join-Path $bundled 'vcpkg.exe')) { $env:VCPKG_ROOT = $bundled }
}
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set and no bundled vcpkg was found.' }

Write-Host "MSVC env loaded from $vsPath; VCPKG_ROOT=$env:VCPKG_ROOT"
