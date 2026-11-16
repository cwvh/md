# Draws photo.jpg, the "photograph" of test/sample.md: smooth gradients that a
# JPEG encoder has real work to do on. The result is committed, so this is
# only needed to make another.
#   powershell -File test/images/photo.ps1

Add-Type -AssemblyName System.Drawing
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$w = 480
$h = 300
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'

function Brush($x0, $y0, $x1, $y1, $c0, $c1) {
    New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new($x0, $y0)), ([System.Drawing.PointF]::new($x1, $y1)), $c0, $c1
}
function C($r, $gr, $b) { [System.Drawing.Color]::FromArgb($r, $gr, $b) }
function P($pairs) {
    $pts = @()
    for ($i = 0; $i -lt $pairs.Count; $i += 2) { $pts += [System.Drawing.PointF]::new($pairs[$i], $pairs[$i + 1]) }
    return $pts
}

# Sky, from deep blue to a warm horizon
$g.FillRectangle((Brush 0 0 0 200 (C 30 60 140) (C 250 190 140)), 0, 0, $w, $h)
# Sun and its glow
for ($i = 6; $i -ge 0; $i--) {
    $r = 28 + $i * 14
    $a = [int](18 + (6 - $i) * 6)
    $b = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($a, 255, 230, 160))
    $g.FillEllipse($b, 340 - $r, 120 - $r, 2 * $r, 2 * $r)
}
$g.FillEllipse((New-Object System.Drawing.SolidBrush (C 255 245 200)), 316, 96, 48, 48)
# Mountains, far to near
$far = P @(0, 200, 70, 130, 130, 170, 210, 100, 290, 180, 380, 120, 480, 190, 480, 300, 0, 300)
$g.FillPolygon((Brush 0 100 0 260 (C 110 120 170) (C 70 80 120)), $far)
$mid = P @(0, 240, 90, 170, 170, 220, 260, 150, 350, 230, 430, 180, 480, 220, 480, 300, 0, 300)
$g.FillPolygon((Brush 0 150 0 280 (C 60 90 100) (C 30 60 70)), $mid)
$near = P @(0, 280, 120, 230, 220, 270, 330, 220, 480, 270, 480, 300, 0, 300)
$g.FillPolygon((Brush 0 220 0 300 (C 30 70 50) (C 10 30 25)), $near)
# A lake
$g.FillEllipse((Brush 0 250 0 300 (C 120 160 190) (C 60 90 120)), 140, 262, 220, 34)
$g.Dispose()

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$params = New-Object System.Drawing.Imaging.EncoderParameters 1
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([long]85)
$bmp.Save((Join-Path $dir 'photo.jpg'), $codec, $params)
$bmp.Dispose()
