param(
    [string]$Output = (Join-Path $PSScriptRoot '..\resources\app.ico')
)

Add-Type -AssemblyName System.Drawing

function New-RoundedPath {
    param([float]$X, [float]$Y, [float]$W, [float]$H, [float]$R)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng([int]$Size) {
    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $margin = [float]($Size * 0.04)
    $side = [float]($Size - 2 * $margin)
    $radius = [float]($Size * 0.22)
    $path = New-RoundedPath $margin $margin $side $side $radius
    $rect = New-Object System.Drawing.RectangleF($margin, $margin, $side, $side)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 59, 130, 246),
        [System.Drawing.Color]::FromArgb(255, 29, 78, 216),
        45.0)
    $graphics.FillPath($brush, $path)

    $white = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)
    $penWidth = [Math]::Max(1.2, $Size * 0.065)
    $pen = New-Object System.Drawing.Pen($white, $penWidth)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $left = [float]($Size * 0.24)
    $right = [float]($Size * 0.76)
    $top = [float]($Size * 0.37)
    $bottom = [float]($Size * 0.63)
    $head = [float]([Math]::Max(1.6, $Size * 0.10))

    $graphics.DrawLine($pen, $left, $top, $right, $top)
    $graphics.DrawLine($pen, $right - $head, $top - $head, $right, $top)
    $graphics.DrawLine($pen, $right - $head, $top + $head, $right, $top)

    $graphics.DrawLine($pen, $right, $bottom, $left, $bottom)
    $graphics.DrawLine($pen, $left + $head, $bottom - $head, $left, $bottom)
    $graphics.DrawLine($pen, $left + $head, $bottom + $head, $left, $bottom)

    $memory = New-Object System.IO.MemoryStream
    $bitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $memory.ToArray()

    $pen.Dispose()
    $brush.Dispose()
    $path.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    $memory.Dispose()
    return $bytes
}

$sizes = @(16, 24, 32, 48, 64, 256)
$images = @()
foreach ($size in $sizes) {
    $images += ,(New-IconPng $size)
}

$outputPath = [System.IO.Path]::GetFullPath($Output)
$directory = [System.IO.Path]::GetDirectoryName($outputPath)
if (!(Test-Path $directory)) {
    New-Item -ItemType Directory -Force $directory | Out-Null
}

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([UInt16]0)
$writer.Write([UInt16]1)
$writer.Write([UInt16]$images.Count)
$offset = 6 + 16 * $images.Count
for ($i = 0; $i -lt $images.Count; $i++) {
    $size = $sizes[$i]
    $writer.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))
    $writer.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))
    $writer.Write([Byte]0)
    $writer.Write([Byte]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]32)
    $writer.Write([UInt32]$images[$i].Length)
    $writer.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) {
    $bytesToWrite = [byte[]]$image
    $writer.Write($bytesToWrite, 0, $bytesToWrite.Length)
}
$writer.Flush()
[System.IO.File]::WriteAllBytes($outputPath, $stream.ToArray())
$writer.Dispose()
$stream.Dispose()

Write-Output "Wrote $outputPath"
