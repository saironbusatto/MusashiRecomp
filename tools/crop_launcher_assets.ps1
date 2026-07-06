# Crop the real launcher art out of the design mockup and knock out the dark
# background to transparent, so each asset blends seamlessly on any panel.
#
# Photo assets (disc/controllers/memcard): edge flood-fill — flood the dark
# background inward from the borders; bright object pixels are walls, so
# interior dark details (disc PlayStation logo, controller buttons, the centre
# hole) are preserved. Logo: simple global luminance key (thin bright glyphs).
Add-Type -AssemblyName System.Drawing
$src = "<desktop>/ef772e04-a7db-4ecd-98bb-eb75a01de0a6.png"
$out = "runtime\launcher\assets\img"
$mock = [System.Drawing.Bitmap]::FromFile($src)

function Get-Crop($x,$y,$w,$h) {
  $rect = New-Object System.Drawing.Rectangle($x,$y,$w,$h)
  return $mock.Clone($rect, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
}

# Returns the byte[] + BitmapData for in-place editing.
function Lock($bmp) {
  $rect = New-Object System.Drawing.Rectangle(0,0,$bmp.Width,$bmp.Height)
  $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadWrite,
                        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $len = $data.Stride * $bmp.Height
  $bytes = New-Object byte[] $len
  [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $len)
  return @{ data=$data; bytes=$bytes; stride=$data.Stride; w=$bmp.Width; h=$bmp.Height }
}
function Unlock($bmp, $ctx) {
  [System.Runtime.InteropServices.Marshal]::Copy($ctx.bytes, 0, $ctx.data.Scan0, $ctx.bytes.Length)
  $bmp.UnlockBits($ctx.data)
}

function FloodTransparent($bmp, $thresh) {
  $c = Lock $bmp
  $b = $c.bytes; $stride = $c.stride; $w = $c.w; $h = $c.h
  $visited = New-Object bool[] ($w*$h)
  $stack = New-Object System.Collections.Generic.Stack[int]
  for ($x=0; $x -lt $w; $x++) { $stack.Push($x); $stack.Push(($h-1)*$w+$x) }
  for ($y=0; $y -lt $h; $y++) { $stack.Push($y*$w); $stack.Push($y*$w+($w-1)) }
  while ($stack.Count -gt 0) {
    $p = $stack.Pop()
    if ($visited[$p]) { continue }
    $visited[$p] = $true
    $px = $p % $w; $py = [int][Math]::Floor($p / $w)
    $i = $py*$stride + $px*4
    $mx = [Math]::Max($b[$i], [Math]::Max($b[$i+1], $b[$i+2]))
    if ($mx -ge $thresh) { continue }   # bright object pixel = wall
    $b[$i+3] = 0                        # background -> transparent
    if ($px -gt 0)    { $n=$p-1;  if(-not $visited[$n]){$stack.Push($n)} }
    if ($px -lt $w-1) { $n=$p+1;  if(-not $visited[$n]){$stack.Push($n)} }
    if ($py -gt 0)    { $n=$p-$w; if(-not $visited[$n]){$stack.Push($n)} }
    if ($py -lt $h-1) { $n=$p+$w; if(-not $visited[$n]){$stack.Push($n)} }
  }
  Unlock $bmp $c
}

function GlobalKey($bmp, $thresh) {
  $c = Lock $bmp
  $b = $c.bytes
  for ($i=0; $i -lt $b.Length; $i+=4) {
    $mx = [Math]::Max($b[$i], [Math]::Max($b[$i+1], $b[$i+2]))
    if ($mx -lt $thresh) { $b[$i+3] = 0 }
  }
  Unlock $bmp $c
}

function Process($x,$y,$w,$h,$name,$mode,$thresh) {
  $bmp = Get-Crop $x $y $w $h
  if ($mode -eq "flood") { FloodTransparent $bmp $thresh } else { GlobalKey $bmp $thresh }
  $bmp.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Output "$name ($mode t=$thresh)"
}

Process 28  20  70  70  "logo.png"        "key"   60
Process 74  120 288 294 "disc.png"        "flood" 74
Process 50  506 346 198 "pad_digital.png" "flood" 80
Process 740 506 344 200 "pad_analog.png"  "flood" 80
Process 56  794 148 164 "memcard.png"     "flood" 80
$mock.Dispose()
Write-Output "done"
