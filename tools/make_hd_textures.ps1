<#
.SYNOPSIS
    Makes the HD texture set from your own disc: every texture of the game upscaled (4K characters,
    2K levels, alpha kept), in hd_textures\ next to this folder's parent, where the native renderer
    finds it on its own (TF_HIRES_DIR overrides).

.DESCRIPTION
    1. Extracts the disc's archives (game\TFA.ZIP, TFA2.ZIP, TA_XTRAS.ZIP, SOUNDE.ZIP) into
       hd_textures\archives (tools\hd_textures\extract_pk_archive.py).
    2. Converts every .ITF texture to PNG, palette variants included, into
       hd_textures\converted\textures (itf_to_png.py). The renderer compares these with the
       textures the game holds to pick the right image for each.
    3. Downloads Real-ESRGAN ncnn-vulkan v0.2.5.0 (MIT licence, runs on any Vulkan GPU: AMD,
       NVIDIA, Intel) into hd_textures\realesrgan, unless it is there already.
    4. Upscales into hd_textures\upscaled (upscale_textures.py: tiers and models in tier_rules.py).
       Resumable: run it again after a stop and it carries on. Then matches each image's colour to
       the game's own texture (match_colour.py: off by more than 5%, one tone curve per channel).
    5. Makes the font atlases again sharp, with PS5 button symbols, into hd_textures\crisp_fonts
       (make_crisp_fonts.py; TODO 119) - the Display settings "Sharp text" and "Button symbols".

    Needs: build.ps1 run once (it extracts the disc into game\), Python 3.11+ with Pillow
    (pip install pillow). Takes from tens of minutes to a few hours, depending on the GPU.
    Everything it writes is game data made from your disc: hd_textures\ is git-ignored, never
    commit or share it.

    Your set will be the same as kayliox's apart from a few hand-made fixes he made later (a
    handful of re-made upscales and redrawn faction insignia), which live in his own copy only.

.EXAMPLE
    .\tools\make_hd_textures.ps1
.EXAMPLE
    .\tools\make_hd_textures.ps1 -SkipUpscale     # steps 1-2 only (and 5)
.EXAMPLE
    .\tools\make_hd_textures.ps1 -CrispFontsOnly  # step 5 only
.EXAMPLE
    .\tools\make_hd_textures.ps1 -FontsOnly      # font assets only, no GPU or downloads
#>
param([switch]$SkipUpscale, [switch]$CrispFontsOnly, [switch]$FontsOnly, [string]$Python = "python")

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Game = Join-Path $Root "game"
$Out = Join-Path $Root "hd_textures"
$Py = Join-Path $PSScriptRoot "hd_textures"

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Run([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = "Continue"
    & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# (TODO 186) numpy, which the sharp fonts need, installed when it is missing; $LASTEXITCODE 0 when it is there.
function EnsureNumpy {
    & $Python -c "import numpy" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Step "Installing numpy (Python), for the sharp fonts"
        & $Python -m pip install --user numpy 2>&1 | ForEach-Object { "$_" }
        & $Python -c "import numpy" 2>$null
    }
}

# Only FONTS from TFA2.ZIP: no other archives, downloads or GPU upscale.
if ($FontsOnly) {
    try {
        $ErrorActionPreference = "Continue"
        EnsureNumpy
        $ErrorActionPreference = "Stop"
        if ($LASTEXITCODE -ne 0) { throw "Python's numpy could not be installed (pip install numpy)" }
        Run $Python @((Join-Path $Py "make_fonts_only.py"), "--game", $Game, "--out", $Out, "--controls", (Join-Path $Root "controls.txt"))
    } catch {
        Write-Host "`nwarning: $($_.Exception.Message): the button prompts and sharp fonts are not made; continuing with the game's own fonts." -ForegroundColor Yellow
    }
    return
}

$zips = "TFA", "TFA2", "TA_XTRAS", "SOUNDE" | ForEach-Object { Join-Path $Game "$_.ZIP" }
foreach ($z in $zips) { if (-not (Test-Path $z)) { throw "$z not found: run build.ps1 -Iso <your disc> first" } }
& $Python -c "import PIL" 2>$null
if ($LASTEXITCODE -ne 0) { throw "Python's Pillow is missing: pip install pillow" }

$archives = Join-Path $Out "archives"
if (-not (Test-Path (Join-Path $archives "TFA"))) {
    Step "Extracting the disc's archives into $archives"
    Run $Python (@((Join-Path $Py "extract_pk_archive.py")) + $zips + @("--out", $archives))
}

$converted = Join-Path $Out "converted\textures"
if (-not (Test-Path (Join-Path $converted "TFA"))) {
    Step "Converting the textures to PNG into $converted"
    $inputs = "TFA", "TFA2", "TA_XTRAS", "SOUNDE" | ForEach-Object { Join-Path $archives $_ } | Where-Object { Test-Path $_ }
    Run $Python (@((Join-Path $Py "itf_to_png.py")) + $inputs + @("--out", $converted, "--all-palettes"))
}

function CrispFonts {
    # 5. (TODO 119) The font atlases made again sharp at 8x - the menu font from TeX Gyre Adventor (GUST Font
    #    License, tools\hd_textures\typeface), the HUD's and HQ's rebuilt with sharp edges - with the button
    #    symbols drawn in the DualSense's style, into hd_textures\crisp_fonts (make_crisp_fonts.py). The
    #    game's Display settings "Sharp text" / "Button symbols" choose among them. Needs numpy; a minute.
    # (TODO 186: numpy installed here when it is missing - setup.ps1 never installed it, so an install made
    # without it never had the sharp text: the game showed the old letters)
    $ErrorActionPreference = "Continue"
    EnsureNumpy
    $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nwarning: Python's numpy could not be installed (pip install numpy): the sharp text and PS5 button symbols are not made." -ForegroundColor Yellow
        return
    }
    Step "Making the sharp fonts and PS5 button symbols into $(Join-Path $Out 'crisp_fonts')"
    $a = @((Join-Path $Py "make_crisp_fonts.py"), "--archives", $archives, "--converted", $converted, "--out", (Join-Path $Out "crisp_fonts"))
    $up = Join-Path $Out "upscaled"
    if (Test-Path $up) { $a += @("--upscaled", $up) }
    $controls = Join-Path $Root "controls.txt"  # (the keyboard layout: the key prompts' keys)
    if (Test-Path $controls) { $a += @("--controls", $controls) }
    Run $Python $a
}

if ($CrispFontsOnly) { CrispFonts; return }
if ($SkipUpscale) { CrispFonts; return }

$esr = Join-Path $Out "realesrgan"
$exe = Join-Path $esr "realesrgan-ncnn-vulkan.exe"
if (-not (Test-Path $exe)) {
    Step "Downloading Real-ESRGAN ncnn-vulkan v0.2.5.0"
    $zip = Join-Path $Out "realesrgan.zip"
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip" -OutFile $zip
    Expand-Archive $zip $esr -Force
    Remove-Item $zip
    if (-not (Test-Path $exe)) { throw "the Real-ESRGAN download did not contain realesrgan-ncnn-vulkan.exe" }
}

Step "Upscaling into $(Join-Path $Out 'upscaled') (resumable)"
# A few textures failing is not a failed setup: the game keeps the original for each texture without
# an upscale, and a run again tries only those. (Exit 1 with its "upscaled N/M" summary = some files
# failed; anything else - no summary, exit 2 - is a real failure.)
$ErrorActionPreference = "Continue"
& $Python (Join-Path $Py "upscale_textures.py") $converted --out (Join-Path $Out "upscaled") `
    --work (Join-Path $Out "work") --exe $exe 2>&1 | ForEach-Object { "$_" } | Tee-Object -Variable lines | Out-Host
$code = $LASTEXITCODE
$ErrorActionPreference = "Stop"
$summary = $lines | Where-Object { $_ -match '^upscaled \d+/\d+ uniques' } | Select-Object -Last 1
if ($code -ne 0) {
    if ($code -eq 1 -and $summary) {
        Write-Host "`nwarning: some textures could not be upscaled ($summary); they keep their original look." -ForegroundColor Yellow
        Write-Host "Running make_hd_textures.ps1 (or setup.ps1) again retries only those." -ForegroundColor Yellow
    }
    else { throw "upscale_textures.py failed with exit code $code" }
}
# 4b. (TODO 124) Each upscale's colour matched to the game's own texture: the upscaler darkens the foliage
#     (10-37%), so the trees and the ground looked unlike the game. An image whose average over its opaque
#     parts is off by more than 5% gets one tone curve per channel (the detail stays as it is); the original is
#     kept in hd_textures\upscaled_precolour (match_colour.py --restore puts it back). Again: no change.
# (Said here, not only by the updater: an update runs the update.ps1 it had before the pull, this script as pulled.)
if (-not (Test-Path (Join-Path $Out "upscaled_precolour\colour_gains.json"))) {
    Step "Matching the HD textures' colours to the game's (one time, about 10 minutes)"
    if (Test-Path (Join-Path $Out "upscaled_bc7_cache")) {  # (a set already played with: its compressed copies are made again)
        Write-Host "Afterwards the first visit to each level loads a little slower, once: its changed textures are compressed again."
    }
}
else { Step "Matching the upscaled images' colour to the game's textures" }
Run $Python @((Join-Path $Py "match_colour.py"), "--upscaled", (Join-Path $Out "upscaled"), "--native", $converted)
CrispFonts
Write-Host "`nDone. Start the game from this folder (Play.bat): the log says '[native render] upscaled textures: N images'." -ForegroundColor Green
