# SPDX-License-Identifier: GPL-3.0-or-later
#
# Downloads the pinned third-party programs Lanes runs into third_party\bin\.
# Every download is checked against a pinned SHA-256; nothing is committed to the repository.
#
#   .\tools\fetch-third-party.ps1            # fetch what is missing
#   .\tools\fetch-third-party.ps1 -Force     # fetch again
#
# To update a pin: take the new release's file name, URL and SHA-256 from the project's
# published checksum list, change the entry below, and run with -Force.

param([switch]$Force)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is much faster without the bar

$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'third_party\bin'
New-Item -ItemType Directory -Force $bin | Out-Null

# Name: file placed in third_party\bin. Zip entries are matched by their path's ending.
$pins = @(
    @{
        Name    = 'ffmpeg.exe'
        # FFmpeg 8.1.3, LGPL build (no GPL-only components), BtbN/FFmpeg-Builds. Dated
        # autobuild releases are removed after some weeks; update the pin when this 404s.
        Url     = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-28-19-47/ffmpeg-n8.1.3-25-g611cec792a-win64-lgpl-8.1.zip'
        Sha256  = '023ba9e491ce616ea2e3c2b10b7c7df16a4714c0ec1dbf0f4e0f59aae04e73e8'
        Entry   = '/bin/ffmpeg.exe'
        License = 'LGPL-2.1-or-later (FFmpeg)'
    },
    @{
        Name    = 'yt-dlp.exe'
        # yt-dlp 2026.10.26 (SHA-256 from the release's SHA2-256SUMS). Lanes updates its own copy
        # in %LOCALAPPDATA%\Lanes\bin at runtime; this is the bundled baseline.
        Url     = 'https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe'
        Sha256  = 'f5389d3698ecde4ef06f4e6b1ce0294fe9ac1396228c8990b8d3416d6d177a7d'
        License = 'Unlicense (yt-dlp)'
    },
    @{
        Name    = 'deno.exe'
        # Deno 2.9.8: the JavaScript runtime yt-dlp needs for YouTube.
        Url     = 'https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-pc-windows-msvc.zip'
        Sha256  = 'cba84b26e0df5ae7f74fc66b86a95b866872a86292ebd84fa1e959927edd93bf'
        Entry   = 'deno.exe'
        License = 'MIT (Deno)'
    }
)

Add-Type -AssemblyName System.IO.Compression.FileSystem

foreach ($pin in $pins) {
    $target = Join-Path $bin $pin.Name
    if ((Test-Path $target) -and -not $Force) {
        Write-Host "$($pin.Name): already present"
        continue
    }
    $download = Join-Path ([System.IO.Path]::GetTempPath()) ("lanes-" + [guid]::NewGuid() + ".download")
    try {
        Write-Host "$($pin.Name): downloading $($pin.Url)"
        $curl = Get-Command curl.exe -ErrorAction SilentlyContinue # in Windows since 1803; fast
        if ($curl) {
            & $curl.Source --fail --location --silent --show-error --output $download $pin.Url
            if ($LASTEXITCODE -ne 0) { throw "$($pin.Name): download failed (curl exit $LASTEXITCODE)" }
        } else {
            Invoke-WebRequest -Uri $pin.Url -OutFile $download -UseBasicParsing
        }
        $hash = (Get-FileHash -Algorithm SHA256 $download).Hash.ToLowerInvariant()
        if ($hash -ne $pin.Sha256) {
            throw "$($pin.Name): SHA-256 mismatch (expected $($pin.Sha256), got $hash)"
        }
        if ($pin.Entry) {
            $zip = [System.IO.Compression.ZipFile]::OpenRead($download)
            try {
                $entry = $zip.Entries | Where-Object { $_.FullName.EndsWith($pin.Entry) } |
                    Select-Object -First 1
                if (-not $entry) { throw "$($pin.Name): $($pin.Entry) not found in the archive" }
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
            } finally {
                $zip.Dispose()
            }
        } else {
            Move-Item -Force $download $target
        }
        Write-Host "$($pin.Name): ok ($($pin.License))"
    } finally {
        if (Test-Path $download) { Remove-Item -Force $download }
    }
}
