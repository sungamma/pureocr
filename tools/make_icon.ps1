<#
.SYNOPSIS
    生成 PureOCR 的窗口/可执行文件图标（resources\pureocr.ico）。

.DESCRIPTION
    画一个圆角方形底（蓝色渐变）+ 白色 "OCR" 字样，输出多尺寸 ICO：
    16/24/32/48/64 用 32bpp DIB（兼容老工具），128/256 用 PNG（Vista+ 常见做法）。

.EXAMPLE
    pwsh -NoProfile -File tools\make_icon.ps1
#>
[CmdletBinding()]
param(
    [string]$OutFile = (Join-Path $PSScriptRoot '..\resources\pureocr.ico')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$dibSizes = @(16, 24, 32, 48, 64)
$pngSizes = @(128, 256)

function New-LogoBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    # 圆角方形底
    $pad = [math]::Max(1, [int]($size * 0.04))
    $r = [int]($size * 0.22)
    $rect = New-Object System.Drawing.Rectangle($pad, $pad, ($size - 2 * $pad), ($size - 2 * $pad))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc(($rect.Right - $d), $rect.Y, $d, $d, 270, 90)
    $path.AddArc(($rect.Right - $d), ($rect.Bottom - $d), $d, $d, 0, 90)
    $path.AddArc($rect.X, ($rect.Bottom - $d), $d, $d, 90, 90)
    $path.CloseFigure()

    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point(0, 0)),
        (New-Object System.Drawing.Point($size, $size)),
        [System.Drawing.Color]::FromArgb(255, 64, 132, 255),
        [System.Drawing.Color]::FromArgb(255, 20, 46, 132))
    $g.FillPath($brush, $path)

    # 顶部高光
    $hl = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point(0, 0)),
        (New-Object System.Drawing.Point(0, $size)),
        [System.Drawing.Color]::FromArgb(55, 255, 255, 255),
        [System.Drawing.Color]::FromArgb(0, 255, 255, 255))
    $g.SetClip($path)
    $g.FillRectangle($hl, 0, 0, $size, $size)
    $g.ResetClip()

    # "OCR" 文字
    $fontSize = [float]($size * 0.36)
    $style = if ($size -ge 32) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular }
    $family = if ($size -ge 32) { 'Arial' } else { 'Segoe UI' }
    $font = New-Object System.Drawing.Font($family, $fontSize, $style, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $sf = New-Object System.Drawing.RectangleF(0, [float]($size * 0.02), $size, $size)
    $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $g.DrawString('OCR', $font, $textBrush, $sf, $fmt)

    $textBrush.Dispose(); $font.Dispose(); $hl.Dispose(); $brush.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

function Get-DibBytes([System.Drawing.Bitmap]$bmp) {
    $size = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    # BITMAPINFOHEADER（高度 = 2 × 实际高度，含 AND 掩码）
    $bw.Write([uint32]40); $bw.Write([int32]$size); $bw.Write([int32]($size * 2))
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
    $bw.Write([uint32]($size * $size * 4)); $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    # XOR 位图（BGRA，自下而上）
    for ($y = $size - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $size; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $bw.Write([byte]$c.B); $bw.Write([byte]$c.G); $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
        }
    }
    # AND 掩码（全 0，透明由 alpha 决定）
    $rowBytes = [int][math]::Ceiling($size / 8.0)
    if ($rowBytes % 4 -ne 0) { $rowBytes += 4 - ($rowBytes % 4) }
    $bw.Write((New-Object byte[] ($rowBytes * $size)))
    $bw.Flush()
    Write-Output -NoEnumerate ([byte[]]$ms.ToArray())
}

$images = @()
foreach ($s in $dibSizes) {
    $bmp = New-LogoBitmap $s
    $images += ,@{ W = $s; Data = [byte[]](Get-DibBytes $bmp); Png = $false }
    $bmp.Dispose()
}
foreach ($s in $pngSizes) {
    $bmp = New-LogoBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += ,@{ W = $s; Data = [byte[]]$ms.ToArray(); Png = $true }
    $bmp.Dispose()
}

$dir = Split-Path -Parent $OutFile
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $w = if ($img.W -ge 256) { 0 } else { $img.W }
    $bw.Write([byte]$w); $bw.Write([byte]$w)
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$img.Data.Length); $bw.Write([uint32]$offset)
    $offset += $img.Data.Length
}
foreach ($img in $images) { $bw.Write($img.Data) }
$bw.Flush()
[IO.File]::WriteAllBytes($OutFile, $out.ToArray())
$bw.Dispose(); $out.Dispose()

"图标已生成：$((Resolve-Path $OutFile).Path)（$([IO.File]::ReadAllBytes($OutFile).Length) 字节，$($images.Count) 个尺寸）"