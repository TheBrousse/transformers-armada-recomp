<#
.SYNOPSIS
    Makes the save states tools\check_frames.ps1 plays from (states\<name>.ps2state), by a scripted cold boot.

.DESCRIPTION
    The states and the kept frames (runs\golden) are not in the repository (git-ignored: a state holds the game's
    memory). If they are lost, this makes the states again; then record the kept set from a known-good build
    (check_frames.ps1 -Update) before checking a change against it. Made 2026-11-04 (cpu6) from mods at 63dab1b
    because the originals were gone, and again for the 256 MB main RAM (N7 step 1: the 32 MB states are refused); kept
    in the main working copy's states\ and runs\golden.

    Each state: a cold boot on a scratch card (run.ps1 -ScratchCard -SaveState), the inputs below, saved at its
    VBlank tick, with the game's picture at that tick beside it (runs\mk_<name>.png) to check the place.
      dropzones   the HQ's Drop Zones ring (the ring turned twice)                         tick 7300
      a13         the Mini-Con loadout screen (the HQ's Mini-Cons ring entered)           tick 7300
      amazon_old  Amazon right after the level starts                                     tick 9000
      a11         Amazon, the walk to the first grunts (they fire)                        tick 10700
      a12         Amazon, the fight by the ruins                                          tick 11100
    Hidden and silent (PS2X_WINDOW_HIDDEN, PS2X_AUDIO_VOLUME=0); at the console's 30 frames a second and draw
    distance 1, as check_frames plays them.

.EXAMPLE
    .\tools\make_check_states.ps1                      # all five, with out\build's exe
    .\tools\make_check_states.ps1 -Only a11,a12 -Exe out\play\TransformersRecomp.exe
#>
param([string]$Exe = "", [string[]]$Only = @())
$Root = Split-Path $PSScriptRoot -Parent
$boot = (1..34 | ForEach-Object { "$($_*150):start" }) + @("5450:cross", "5700:cross", "5950:cross", "6200:cross")
# (Amazon: the episode, its warp-gate list, start; the camera looks around, the walk to the grunts at 10500)
$lvl = @("6900:right", "7100:right", "7300:cross", "7500:cross", "7750:cross", "8350:start")
$gate = @("7530:left", "7570:left", "7610:left", "7650:left", "7690:left", "7730:left")
$fight = @("9000:r3", "9100:rleft@40:250", "9450:rright@40:250", "9850:r3") + (0..5 | ForEach-Object { "$(11000 + 60*$_):square" })
$states = [ordered]@{
    dropzones  = @{ At = 7300; Input = $boot + @("6650:right", "6850:right") }
    a13        = @{ At = 7300; Input = $boot + @("6650:right", "6850:cross") }
    amazon_old = @{ At = 9000; Input = $boot + $lvl + $gate }
    a11        = @{ At = 10700; Input = $boot + $lvl + $gate + $fight; Goto = "10500:-280,-835,8" }
    a12        = @{ At = 11100; Input = $boot + $lvl + $gate + $fight; Goto = "10500:-280,-835,8" }
}
Push-Location $Root
try {
    New-Item -ItemType Directory -Force "runs", "states" | Out-Null
    foreach ($name in $states.Keys) {
        if ($Only.Count -and $Only -notcontains $name) { continue }
        $s = $states[$name]
        $vars = @{ TF_INPUT = ($s.Input -join ","); TF_SKIP_FMV = "1"; TF_UNLOCK_ALL = "3300"; TF_GOD = "1"; TF_60FPS = "0";
            TF_DRAW_DISTANCE = "1"; PS2X_TURBO_VSYNCS = "100000000"; TF_QUIT_VSYNC = "$($s.At + 20)"; PS2X_SHOT_VSYNCS = "$($s.At)";
            PS2X_WINDOW_HIDDEN = "1"; PS2X_AUDIO_VOLUME = "0"; TRANSFORMERS_GAME_DIR = (Join-Path $Root "game") }
        if ($s.Goto) { $vars.TF_GOTO = $s.Goto }
        foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
        $runArgs = @{ Seconds = 600; Name = "mk_$name"; ScratchCard = $true; SaveState = $name; SaveAt = $s.At }
        if ($Exe) { $runArgs.Exe = $Exe }
        .\run.ps1 @runArgs *> "runs\mk_$name.console.txt"
        foreach ($k in $vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
        if (Test-Path "runs\shot_$($s.At).png") { Move-Item -Force "runs\shot_$($s.At).png" "runs\mk_$name.png" }
        "${name}: $(if (Test-Path "states\$name.ps2state") { 'written' } else { 'NOT written (runs\mk_' + $name + '.err)' })"
    }
}
finally { Pop-Location }
