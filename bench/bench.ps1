# Times the C runtime on your own screenshots and prints a table: per tier (Small / Normal / Large), the plain C build,
# the AVX2 build on one thread (lightest) and the AVX2 build on several threads (fastest), plus the same two for the
# native build when build.bat native made one. First it checks every build against the scalar one bit for bit
# (parity.exe: every recognizer output and finder map row over line crops of the frames); a mismatch fails the run.
#
#   powershell -ExecutionPolicy Bypass -File bench.ps1 [-Frames DIR] [-Repeat 20] [-Threads 8]
#
# Frames: every .png / .jpg / .bmp / .ppm / .pgm in DIR (default: frames\ here), e.g. a few game screenshots with
# dialogue. Each is read the way the table in the README was made: greedy, no LM, no cascade, -Repeat warm reads per
# frame. A cell is the mean over frames of the median read; peak RAM is the largest peak working set over the frames.
param(
    [string]$Frames = (Join-Path $PSScriptRoot 'frames'),
    [int]$Repeat = 20,
    [int]$Threads = [Math]::Min(8, [Environment]::ProcessorCount)
)
$ErrorActionPreference = 'Stop'
# the image conversion below needs Windows PowerShell's System.Drawing: from PowerShell 7, run there instead
if ($PSVersionTable.PSEdition -eq 'Core') {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Frames $Frames -Repeat $Repeat -Threads $Threads
    exit $LASTEXITCODE
}
$build = Join-Path $PSScriptRoot 'build'
$models = Join-Path $PSScriptRoot 'models'
foreach ($f in @("$build\ocr_page.exe", "$build\ocr_page_scalar.exe", "$models\textspot8_t64n8c16_w8.vtsp")) {
    if (-not (Test-Path $f)) { throw "$f missing: run fetch_models.ps1 and build.bat first" }
}

# ocr_page reads binary PPM / PGM only: convert the other images into build\frames\
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
public static class Ppm {
    public static void Write(string src, string dst) {
        using (var bmp = new Bitmap(src)) {
            int w = bmp.Width, h = bmp.Height;
            var d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            var row = new byte[d.Stride];
            var rgb = new byte[w * 3 * h];
            for (int y = 0; y < h; y++) {
                Marshal.Copy(d.Scan0 + y * d.Stride, row, 0, d.Stride);
                for (int x = 0; x < w; x++) {
                    rgb[(y * w + x) * 3] = row[x * 3 + 2];
                    rgb[(y * w + x) * 3 + 1] = row[x * 3 + 1];
                    rgb[(y * w + x) * 3 + 2] = row[x * 3];
                }
            }
            bmp.UnlockBits(d);
            using (var o = File.Create(dst)) {
                var hd = System.Text.Encoding.ASCII.GetBytes("P6\n" + w + " " + h + "\n255\n");
                o.Write(hd, 0, hd.Length);
                o.Write(rgb, 0, rgb.Length);
            }
        }
    }
}
'@
$conv = Join-Path $build 'frames'
New-Item -ItemType Directory -Force $conv | Out-Null
$list = @()
foreach ($f in Get-ChildItem $Frames -File | Sort-Object Name) {
    $ext = $f.Extension.ToLower()
    if ($ext -eq '.ppm' -or $ext -eq '.pgm') { $list += $f.FullName }
    elseif ('.png', '.jpg', '.jpeg', '.bmp' -contains $ext) {
        $dst = Join-Path $conv ($f.BaseName + '.ppm')
        [Ppm]::Write($f.FullName, $dst)
        $list += $dst
    }
}
if (-not $list) { throw "no images in ${Frames}: put a few screenshots there, or pass -Frames DIR" }

function Median($a) {
    $s = @($a | Sort-Object)
    if ($s.Count % 2) { return $s[($s.Count - 1) / 2] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

$fast = @('--threads', $Threads, '--tile', 64, '--head', 64)
$setups = [ordered]@{
    'C scalar'                                    = @("$build\ocr_page_scalar.exe")
    'C AVX2, lightest (1 thread)'                 = @("$build\ocr_page.exe")
    "C AVX2, fastest ($Threads threads, tile 64)" = @("$build\ocr_page.exe") + $fast
}
$parity = [ordered]@{
    'C AVX2, 1 thread'           = @("$build\parity.exe")
    "C AVX2, $Threads threads"   = @("$build\parity.exe") + $fast
}
if (Test-Path "$build\ocr_page_native.exe") { # build.bat native
    $setups['C native, lightest (1 thread)'] = @("$build\ocr_page_native.exe")
    $setups["C native, fastest ($Threads threads, tile 64)"] = @("$build\ocr_page_native.exe") + $fast
    $parity['C native, 1 thread'] = @("$build\parity_native.exe")
    $parity["C native, $Threads threads"] = @("$build\parity_native.exe") + $fast
}

# bit-for-bit check first: every recognizer output and every finder map row must hash the same as the scalar build's
Write-Host "parity check against the scalar build"
$want = & "$build\parity_scalar.exe" --models $models @list
if ($LASTEXITCODE) { throw "parity_scalar.exe failed" }
$same = $true
foreach ($name in $parity.Keys) {
    $exe = $parity[$name][0]
    $extra = @($parity[$name] | Select-Object -Skip 1)
    $got = & $exe --models $models @extra @list
    if ($LASTEXITCODE) { throw "$exe failed" }
    if (($got -join "`n") -eq ($want -join "`n")) { Write-Host "  same      $name" }
    else {
        $same = $false
        Write-Warning "$name differs from the scalar build:"
        for ($i = 0; $i -lt $want.Count; $i++) { if ($got[$i] -ne $want[$i]) { Write-Host "    want $($want[$i])`n    got  $($got[$i])" } }
    }
}

Write-Host "$($list.Count) frame(s), $Repeat reads each, $([Environment]::ProcessorCount) logical CPUs"
$rows = @()
$texts = @{}
foreach ($t in 'S', 'N', 'L') {
    foreach ($name in $setups.Keys) {
        $exe = $setups[$name][0]
        $extra = @($setups[$name] | Select-Object -Skip 1)
        $page = 0.0; $spot = 0.0; $rec = 0.0; $lines = 0; $peak = 0
        foreach ($f in $list) {
            $out = & $exe $f --models $models --tier $t --no-lm --no-cascade --repeat $Repeat @extra
            if ($LASTEXITCODE) { throw "$exe failed on $f" }
            $j = ($out -join "`n") | ConvertFrom-Json
            $tot = @($j.warm.total_ms); $sp = @($j.warm.spot_ms)
            $page += Median $tot
            $spot += Median $sp
            $rec += Median (0..($tot.Count - 1) | ForEach-Object { $tot[$_] - $sp[$_] })
            $lines += @($j.lines).Count
            $peak = [Math]::Max($peak, [double]$j.process.peak_working_set)
            $key = "$t|$f"
            $txt = (@($j.lines) | ForEach-Object { $_.greedy }) -join "`n"
            if (-not $texts.ContainsKey($key)) { $texts[$key] = $txt }
            elseif ($texts[$key] -ne $txt) { Write-Warning "$name, tier $t, $f`: text differs from the scalar build's" }
        }
        $n = $list.Count
        $rows += [pscustomobject]@{
            tier = $t; build = $name
            'per line ms' = if ($lines) { '{0:N2}' -f ($rec / $lines) } else { '-' }
            'line finder ms' = '{0:N1}' -f ($spot / $n)
            'page ms' = '{0:N1}' -f ($page / $n)
            'peak RAM' = '{0:N1} MiB' -f ($peak / 1MB)
        }
    }
    Write-Host "tier $t done"
}
$rows | Format-Table -AutoSize
if (-not $same) { Write-Warning 'parity FAILED: a build does not match the scalar build bit for bit (see above)'; exit 1 }
