# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Windows installer build\installer\lanes-setup-<version>.exe:
# release build → `cmake --install` into build\stage → optional signing → Inno Setup.
#
#   powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 [-SkipBuild] [-Iscc <path>]
#
# Needs: Visual Studio with C++ (tools\dev-shell.ps1), the Qt MSVC kit, Inno Setup 6, and the
# third-party programs (tools\fetch-third-party.ps1, run automatically when missing).
#
# Signing happens only when LANES_SIGN_ARGS is set, e.g.
#   $env:LANES_SIGN_ARGS = '/fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /f cert.pfx /p ...'
# Lanes' own executables and the installer get signed; third-party programs (Qt, ffmpeg, yt-dlp,
# Deno) keep their own signatures. Without it the installer is unsigned and the build still works.

param(
    [switch]$SkipBuild,
    [string]$Iscc = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$preset = 'windows-release'
$build = Join-Path $root "build\$preset"
$stage = Join-Path $root 'build\stage'
$output = Join-Path $root 'build\installer'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    . (Join-Path $PSScriptRoot 'dev-shell.ps1')
}

foreach ($program in 'ffmpeg.exe', 'yt-dlp.exe', 'deno.exe') {
    if (-not (Test-Path (Join-Path $root "third_party\bin\$program"))) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'fetch-third-party.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'fetching the third-party programs failed' }
        break
    }
}

Push-Location $root
try {
    if (-not $SkipBuild) {
        cmake --preset $preset
        if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
        cmake --build --preset $preset
        if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    }
    if (-not (Test-Path (Join-Path $build 'bin\lanes-gui.exe'))) {
        throw 'lanes-gui.exe was not built: is the Qt MSVC kit installed (C:\Qt\6.x\msvc*_64)?'
    }

    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    cmake --install $build --prefix $stage
    if ($LASTEXITCODE -ne 0) { throw 'install (staging) failed' }

    $signArgs = $env:LANES_SIGN_ARGS
    $isccSign = @()
    if ($signArgs) {
        $signtool = (Get-Command signtool.exe -ErrorAction Stop).Source
        foreach ($exe in 'lanes.exe', 'lanes-gui.exe', 'lanes-bridge.exe') {
            $cmd = "& `"$signtool`" sign $signArgs `"$(Join-Path $stage $exe)`""
            Invoke-Expression $cmd
            if ($LASTEXITCODE -ne 0) { throw "signing $exe failed" }
        }
        # Inno Setup signs the setup and the uninstaller with this named tool ($f = the file).
        $isccSign = @("/Slanes=`"$signtool`" sign $signArgs `$f", '/DSignTool=lanes')
    } else {
        Write-Host 'LANES_SIGN_ARGS is not set: building an unsigned installer'
    }

    if (-not $Iscc) {
        foreach ($candidate in "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
                               "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
                               "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") {
            if (Test-Path $candidate) { $Iscc = $candidate; break }
        }
    }
    if (-not $Iscc -or -not (Test-Path $Iscc)) {
        throw 'Inno Setup 6 (ISCC.exe) was not found: install it from https://jrsoftware.org or pass -Iscc'
    }

    $version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^\s*VERSION\s+(\S+)').Matches[0].Groups[1].Value
    New-Item -ItemType Directory -Force $output | Out-Null
    & $Iscc /Qp "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$output" @isccSign (Join-Path $root 'installer\lanes.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
    Write-Host "Installer: $(Join-Path $output "lanes-setup-$version.exe")"
} finally {
    Pop-Location
}
