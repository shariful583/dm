# SPDX-License-Identifier: GPL-3.0-or-later
#
# Packs the browser extension for the Chrome Web Store and Edge Add-ons:
# build\extension\lanes-extension-<version>.zip.
#
#   powershell -ExecutionPolicy Bypass -File tools\make-extension.ps1
#
# The stores assign their own extension ID and reject a manifest with "key", so the dev key
# (which only pins the unpacked ID) is left out. Entry names use '/' as the stores require
# (Compress-Archive in Windows PowerShell writes '\').

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'extension'
$output = Join-Path $root 'build\extension'

$manifest = [IO.File]::ReadAllText((Join-Path $source 'manifest.json'))
if ($manifest -notmatch '"version"\s*:\s*"([^"]+)"') { throw 'manifest.json has no version' }
$version = $Matches[1]
$storeManifest = [regex]::Replace($manifest, '(?m)^\s*"key"\s*:\s*"[^"]*",\r?\n', '')

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force $output | Out-Null
$zipPath = Join-Path $output "lanes-extension-$version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath }

$zip = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    foreach ($file in Get-ChildItem $source -Recurse -File) {
        $name = $file.FullName.Substring($source.Length + 1).Replace('\', '/')
        if ($name -eq 'manifest.json') {
            $entry = $zip.CreateEntry($name, 'Optimal')
            $writer = New-Object IO.StreamWriter($entry.Open(), (New-Object Text.UTF8Encoding($false)))
            try { $writer.Write($storeManifest) } finally { $writer.Dispose() }
        } else {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $name, 'Optimal') | Out-Null
        }
    }
} finally {
    $zip.Dispose()
}
Write-Host "Extension package: $zipPath"
