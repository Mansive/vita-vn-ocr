# Downloads the models the benchmark reads from release v0.5.0 into bench\models\, checking each file's SHA-256
# (the release's SHA256SUMS). Files already there with the right hash are kept.
#
#   powershell -ExecutionPolicy Bypass -File fetch_models.ps1
#
# The weights are for non-commercial use only: see the release's notes and attribution files,
# https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.5.0
$ErrorActionPreference = 'Stop'
$base = 'https://github.com/bee-san/vita-vn-ocr/releases/download/v0.5.0'
$files = [ordered]@{
    'VOCR6.2_S_w8.vocr'          = '864bbd196d5bc5d3a7bd7918cf7274b3503dfe65d440017ef0781ea7e7ba37d2'
    'VOCR6.2_N_w8g2.vocr'        = '107cf084cc0c1a3a61ce4c3f9b9cf6b31d6dff80f0a1bdb94afd3f7ee9ae3bc9'
    'VOCR6.2_L_w8_2l.vocr'       = '65310013e8f990886e4e8097b2fe55eb98cdf082d7193eea510601fc0ac26c82'
    'textspot8_t64n8c16_w8.vtsp' = 'e129d41adfa02c658a7d13d6f98064e6ab6ce9a2bc8c7a56da700a21357b5e1c'
    'vocr_lm6_o3_1000k.vlm'      = '41591accf8499f1c34557e585cca382de2c4b5ca15f270c985612c32487e251f'
}
$dir = Join-Path $PSScriptRoot 'models'
New-Item -ItemType Directory -Force $dir | Out-Null
foreach ($name in $files.Keys) {
    $path = Join-Path $dir $name
    if ((Test-Path $path) -and (Get-FileHash $path -Algorithm SHA256).Hash -eq $files[$name]) {
        Write-Host "ok        $name"
        continue
    }
    Write-Host "download  $name"
    Invoke-WebRequest -Uri "$base/$name" -OutFile "$path.part" -UseBasicParsing
    if ((Get-FileHash "$path.part" -Algorithm SHA256).Hash -ne $files[$name]) {
        Remove-Item "$path.part"
        throw "$name`: SHA-256 does not match the release's"
    }
    Move-Item -Force "$path.part" $path
}
Write-Host "models in $dir"
