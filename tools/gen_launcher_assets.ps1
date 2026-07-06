# Generates placeholder launcher art into runtime/launcher/assets/img.
# Stylized reproductions of the mockup (disc, controllers, memory card, PS-glyph
# logo, verification icons). Swap with real renders later — the <img>/decorator
# pipeline (stb_image PNG loader) is the same either way.
Add-Type -AssemblyName System.Drawing
$out = "runtime\launcher\assets\img"
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force $out | Out-Null }

function New-Canvas($w, $h) {
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)
  return $bmp, $g
}
function Save($bmp, $name) {
  $p = Join-Path $out $name
  $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
  Write-Output "wrote $name"
}
function RGB($r,$g,$b) { [System.Drawing.Color]::FromArgb(255,$r,$g,$b) }
function ARGB($a,$r,$g,$b) { [System.Drawing.Color]::FromArgb($a,$r,$g,$b) }

# ---- check icons (18) ----
function Make-Check($name, $on) {
  $bmp,$g = New-Canvas 36 36
  if ($on) {
    $br = New-Object System.Drawing.SolidBrush (RGB 63 185 80)
    $g.FillEllipse($br, 1, 1, 34, 34)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), 3.5
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLines($pen, @((New-Object System.Drawing.PointF(10,18)),
                         (New-Object System.Drawing.PointF(16,24)),
                         (New-Object System.Drawing.PointF(26,12))))
  } else {
    $pen = New-Object System.Drawing.Pen (RGB 60 70 84), 2.5
    $g.DrawEllipse($pen, 2, 2, 32, 32)
  }
  Save $bmp $name
}
Make-Check "check_on.png" $true
Make-Check "check_off.png" $false

# ---- verdict icons (50) ----
function Make-Verdict($name, $col, $kind) {
  $bmp,$g = New-Canvas 100 100
  $pen = New-Object System.Drawing.Pen $col, 5
  $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawEllipse($pen, 6, 6, 88, 88)
  switch ($kind) {
    "ok"   { $g.DrawLines($pen, @((New-Object System.Drawing.PointF(30,52)),
                                   (New-Object System.Drawing.PointF(45,67)),
                                   (New-Object System.Drawing.PointF(72,33)))) }
    "warn" { $g.DrawLine($pen,50,28,50,60); $g.FillEllipse((New-Object System.Drawing.SolidBrush $col),46,68,8,8) }
    "bad"  { $g.DrawLine($pen,36,36,64,64); $g.DrawLine($pen,64,36,36,64) }
    "none" { $g.DrawLine($pen,38,40,62,40) }
  }
  Save $bmp $name
}
Make-Verdict "verdict_ok.png"   (RGB 63 185 80)  "ok"
Make-Verdict "verdict_warn.png" (RGB 210 153 34) "warn"
Make-Verdict "verdict_bad.png"  (RGB 248 81 73)  "bad"
Make-Verdict "verdict_none.png" (RGB 110 119 129) "none"

# ---- PS-glyph logo (46) ----
$bmp,$g = New-Canvas 92 92
$pen = New-Object System.Drawing.Pen (RGB 0 0 0), 5
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
# square (top-left, pink)
$pen.Color = RGB 233 92 138; $g.DrawRectangle($pen, 10, 10, 28, 28)
# triangle (top-right, teal)
$pen.Color = RGB 45 212 191
$g.DrawPolygon($pen, @((New-Object System.Drawing.PointF(68,10)),
                       (New-Object System.Drawing.PointF(54,38)),
                       (New-Object System.Drawing.PointF(82,38))))
# X (bottom-left, purple)
$pen.Color = RGB 137 87 229
$g.DrawLine($pen,12,56,38,82); $g.DrawLine($pen,38,56,12,82)
# circle (bottom-right, red)
$pen.Color = RGB 239 83 110; $g.DrawEllipse($pen, 54, 54, 28, 28)
Save $bmp "logo.png"

# ---- disc (200) ----
$bmp,$g = New-Canvas 240 240
$silver = RGB 196 201 208
$g.FillEllipse((New-Object System.Drawing.SolidBrush $silver), 6, 6, 228, 228)
$pen = New-Object System.Drawing.Pen (RGB 150 156 165), 2
$g.DrawEllipse($pen, 6, 6, 228, 228)
# subtle ring
$g.DrawEllipse((New-Object System.Drawing.Pen ((ARGB 90 255 255 255), 1)), 40, 40, 160, 160)
# hub + hole
$g.FillEllipse((New-Object System.Drawing.SolidBrush (RGB 170 176 184)), 78, 78, 84, 84)
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Transparent)), 104, 104, 32, 32)
$g.FillEllipse((New-Object System.Drawing.SolidBrush (RGB 11 14 20)), 104, 104, 32, 32)
# faint inner data ring for a subtle CD sheen
$g.DrawEllipse((New-Object System.Drawing.Pen ((ARGB 60 255 255 255), 1)), 64, 64, 112, 112)
Save $bmp "disc.png"

# ---- controller ----
function Make-Pad($name, $analog) {
  $bmp,$g = New-Canvas 280 200
  $body = New-Object System.Drawing.SolidBrush (RGB 154 161 171)
  $dark = New-Object System.Drawing.SolidBrush (RGB 64 70 82)
  # grips + body (rounded rects approximated with ellipse caps)
  $g.FillEllipse($body, 8, 70, 90, 120)    # left grip
  $g.FillEllipse($body, 182, 70, 90, 120)  # right grip
  $g.FillRectangle($body, 60, 60, 160, 80) # center
  $g.FillEllipse($body, 40, 55, 90, 90)
  $g.FillEllipse($body, 150, 55, 90, 90)
  # d-pad (left)
  $g.FillRectangle($dark, 70, 88, 36, 12)
  $g.FillRectangle($dark, 82, 76, 12, 36)
  # face buttons (right) — PS colors
  $g.FillEllipse((New-Object System.Drawing.SolidBrush (RGB 233 92 138)), 188, 82, 14, 14) # square pink
  $g.FillEllipse((New-Object System.Drawing.SolidBrush (RGB 45 212 191)), 200, 70, 14, 14) # triangle teal
  $g.FillEllipse((New-Object System.Drawing.SolidBrush (RGB 239 83 110)), 212, 82, 14, 14) # circle red
  $g.FillEllipse((New-Object System.Drawing.SolidBrush (RGB 137 87 229)), 200, 94, 14, 14) # x purple
  if ($analog) {
    $g.FillEllipse($dark, 104, 120, 30, 30)
    $g.FillEllipse($dark, 150, 120, 30, 30)
  }
  Save $bmp $name
}
Make-Pad "pad_digital.png" $false
Make-Pad "pad_analog.png"  $true

# ---- memory card (110x132) ----
$bmp,$g = New-Canvas 220 264
$body = New-Object System.Drawing.SolidBrush (RGB 150 157 167)
$g.FillRectangle($body, 20, 16, 180, 230)
# label area
$g.FillRectangle((New-Object System.Drawing.SolidBrush (RGB 176 182 191)), 36, 40, 148, 120)
# connector notch
$g.FillRectangle((New-Object System.Drawing.SolidBrush (RGB 64 70 82)), 70, 210, 80, 22)
Save $bmp "memcard.png"

# ---- dropdown caret (16x16, downward triangle) ----
$bmp,$g = New-Canvas 32 32
$tri = New-Object System.Drawing.SolidBrush (RGB 173 186 199)
$g.FillPolygon($tri, @((New-Object System.Drawing.PointF(7,11)),
                       (New-Object System.Drawing.PointF(25,11)),
                       (New-Object System.Drawing.PointF(16,22))))
Save $bmp "caret.png"

Write-Output "done"
