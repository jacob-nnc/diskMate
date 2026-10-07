Add-Type -AssemblyName System.Drawing
$src = "D:\Desktop\diskmate\icon\icon_export.png"
$ico = "D:\Desktop\diskmate\icon\diskmate_tree.ico"
$bmp = [System.Drawing.Bitmap]::FromFile($src)
$sizes = @(256,128,64,48,32,16)
$pngs = @()
foreach ($s in $sizes) {
  $nb = New-Object System.Drawing.Bitmap($s, $s)
  $g = [System.Drawing.Graphics]::FromImage($nb)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.DrawImage($bmp, 0, 0, $s, $s)
  $ms = New-Object System.IO.MemoryStream
  $nb.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $pngs += ,$ms.ToArray()
  $g.Dispose(); $nb.Dispose(); $ms.Dispose()
}
$bmp.Dispose()
$fs = New-Object System.IO.FileStream($ico, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)                    # reserved
$bw.Write([uint16]1)                    # type = icon
$bw.Write([uint16]$sizes.Count)         # image count
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]
  $bw.Write([byte]($(if ($s -eq 256) { 0 } else { $s })))  # width
  $bw.Write([byte]($(if ($s -eq 256) { 0 } else { $s })))  # height
  $bw.Write([byte]0)                    # color count
  $bw.Write([byte]0)                    # reserved
  $bw.Write([uint16]1)                  # planes
  $bw.Write([uint16]32)                 # bpp
  $bw.Write([uint32]$pngs[$i].Length)
  $bw.Write([uint32]$offset)
  $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close(); $fs.Close()
"ICO written: $ico"
Get-Item $ico | Select-Object Name,Length