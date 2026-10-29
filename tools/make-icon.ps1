# SPDX-License-Identifier: GPL-3.0-or-later
#
# Draws the Lanes icon (three lanes flowing into a down arrow) and writes src\gui\resources\
# lanes.ico with PNG frames at the sizes Windows asks for. Run it again after changing the
# drawing:   powershell -File tools\make-icon.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'src\gui\resources\lanes.ico'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null

function New-RoundedRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = [single]$size

    # Background: rounded square, blue diagonal gradient.
    $inset = [Math]::Max(0.5, $s * 0.04)
    $bg = New-RoundedRect $inset $inset ($s - 2 * $inset) ($s - 2 * $inset) ($s * 0.22)
    $top = [System.Drawing.Color]::FromArgb(255, 56, 132, 255)
    $bottom = [System.Drawing.Color]::FromArgb(255, 20, 64, 196)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, 0)), (New-Object System.Drawing.PointF($s, $s)), $top, $bottom)
    $g.FillPath($brush, $bg)

    # Three lanes (rounded bars) ...
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $barW = [Math]::Max(1.5, $s * 0.11)
    $barTop = $s * 0.17
    $barH = $s * 0.36
    foreach ($cx in @(0.30, 0.50, 0.70)) {
        $bar = New-RoundedRect ($s * $cx - $barW / 2) $barTop $barW $barH ($barW / 2)
        $g.FillPath($white, $bar)
    }
    # ... flowing into one down arrow.
    $arrow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $arrow.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF(($s * 0.20), ($s * 0.56))),
        (New-Object System.Drawing.PointF(($s * 0.80), ($s * 0.56))),
        (New-Object System.Drawing.PointF(($s * 0.50), ($s * 0.84)))))
    $g.FillPath($white, $arrow)

    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
$frames = @($sizes | ForEach-Object { , (New-IconPng $_) })

# ICONDIR + ICONDIRENTRY[n] + PNG data.
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$frames[$i].Length); $w.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($f in $frames) { $w.Write($f) }
$w.Dispose()
Write-Host "wrote $out ($((Get-Item $out).Length) bytes)"
