<#
.SYNOPSIS
    Runs one scripted scene with each optimized path switched off in turn and compares the
    frame with the default build's: tells which path, if any, a picture problem comes from.

.DESCRIPTION
    Runs from a save state in turbo mode (repeatable), with the given TF_INPUT, and captures
    the frame at -Shot. Variants: the scalar rasterizer loops, the reference rasterizer, VU1
    with all flags, VU1 without the JIT, the VU1 reference interpreter, one rasterizer thread.
    A variant whose frame equals the default's draws the same picture.

.EXAMPLE
    .\tools\compare_paths.ps1 -TfInput "6660:rright:4" -Shot 6674
#>
param(
    [string]$TfInput = "",
    [int]$Shot = 6700,
    [string]$State = "amazon",
    [string[]]$Variants = @("PS2X_GS_SIMD=0", "PS2X_VU1_ALL_FLAGS=1", "PS2X_VU1_JIT=0", "PS2X_VU1_REFERENCE=1", "PS2X_GS_THREADS=1", "PS2X_GS_REFERENCE=1")
)
$Root = Split-Path $PSScriptRoot -Parent
Push-Location $Root
try {
    $out = "runs\compare_paths"
    New-Item -ItemType Directory -Force $out | Out-Null
    $env:PS2X_TURBO_VSYNCS = "13000"
    $env:PS2X_SHOT_VSYNCS = "$Shot"
    $env:TF_QUIT_VSYNC = "$($Shot + 10)"
    $env:TF_INPUT = $TfInput
    # (the paths compared are the software renderer's)
    .\run.ps1 -Seconds 300 -Name compare_paths -LoadState $State -Software *> runs\compare_paths.console.txt
    Copy-Item "runs\shot_$Shot.png" "$out\default.png" -Force
    foreach ($variant in $Variants) {
        $name, $value = $variant.Split("=")
        Set-Item "Env:$name" $value
        .\run.ps1 -Seconds 300 -Name compare_paths -LoadState $State -Software *> runs\compare_paths.console.txt
        Remove-Item "Env:$name"
        $frame = "$out\$name.png"
        if (Test-Path "runs\shot_$Shot.png") {
            Copy-Item "runs\shot_$Shot.png" $frame -Force
            $result = python tools\frame_diff.py "$out\default.png" $frame | Select-Object -First 2
            "{0,-22} {1}" -f $variant, ($result -join "; ")
        } else {
            "{0,-22} no frame" -f $variant
        }
    }
}
finally { Pop-Location }
