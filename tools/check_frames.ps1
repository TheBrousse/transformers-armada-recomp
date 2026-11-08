<#
.SYNOPSIS
    Regression check for the picture: plays fixed stretches of the game from save states in
    turbo mode (which is repeatable) and compares frames with a kept set, pixel for pixel.

.DESCRIPTION
    Cases (each has its own folder under runs\golden\):
      level     Amazon Basin right after the level starts (state amazon_old)
      dropship  the walk from the dropship to the first Mini-Con, enemies firing (state a11)
      minicon   a fight next to the first Mini-Con (state a12)
      room      the Drop Zones room with its warp gate movie (state dropzones)
      menu      the Mini-Con pickup menu (state a13)

    Both renderers draw (PS2X_GPU_MIRROR=1, no texture pack): the software renderer's frames
    are compared with the kept set, the GPU's with the software renderer's
    (tools\gpu_compare.py).

    WHERE THEY LIVE: the states (states\<state>.ps2state) and the kept set (runs\golden\<case>\) are
    git-ignored, kept in the main working copy (its states\ and runs\golden\) - copy them into a worktree before
    running there. The originals were lost; they
    were made again on 2026-11-04 by tools\make_check_states.ps1 (mods at 63dab1b), and again for the
    256 MB main RAM (N7 step 1, branch n7: a state holds all of it, ~270 MB, and states of the 32 MB
    builds are refused), the kept set recorded from that build with -Update -Set @{ PS2X_MTVU = "0" }
    (VIF1 on the EE thread, so builds of different speed compare). If they are lost again: make_check_states.ps1, then -Update
    from a known-good build. A build outside the repository (-Exe) needs TRANSFORMERS_GAME_DIR (in -Set)
    to find the game.

.EXAMPLE
    .\tools\check_frames.ps1                      # all cases
    .\tools\check_frames.ps1 -Case dropship
    .\tools\check_frames.ps1 -Update              # keep this build's frames as the new set

    With VIF1 on the EE thread (PS2X_MTVU=0) a run does not depend on how fast the threads are,
    so two builds that should do the same can be compared even when one is faster:
    .\tools\check_frames.ps1 -Exe out\play\TransformersRecomp.exe -Set @{ PS2X_MTVU = "0" } -GoldenRoot runs\golden_mtvu0 -Update
    .\tools\check_frames.ps1 -Set @{ PS2X_MTVU = "0" } -GoldenRoot runs\golden_mtvu0
#>
param(
    [string[]]$Case = @("level", "dropship", "minicon", "room", "menu"),
    [string]$Exe = "",    # another build to check (e.g. out\play\TransformersRecomp.exe), else out\build's
    [switch]$Update,
    [hashtable]$Set = @{},        # extra environment variables for the runs
    [string]$GoldenRoot = "runs\golden"   # the kept sets (a folder per case)
)
$Root = Split-Path $PSScriptRoot -Parent
$way = "20:468.5,-1743.7,2;20:465.5,-1740.7,2;20:462.5,-1740.7,2;20:459.5,-1739.7,2;20:456.5,-1737.7,2;20:450.5,-1737.7,2;20:444.5,-1737.7,2;20:441.5,-1737.7,2;20:436.5,-1732.7,2;20:431.5,-1727.7,2;20:428.5,-1725.7,2;20:427.5,-1724.7,2"
$cases = @{
    level    = @{ State = "amazon_old"; Offsets = @(50, 100, 150, 200) }
    dropship = @{ State = "a11"; Offsets = @(60, 150, 240, 330, 420); Goto = $way; God = $true }
    minicon  = @{ State = "a12"; Offsets = @(100, 400, 800, 1200, 1700, 2300); God = $true; Do = "60:cross:4,1500:select:4,2000:cross:4" }
    room     = @{ State = "dropzones"; Offsets = @(40, 120, 400, 700) }
    menu     = @{ State = "a13"; Offsets = @(30, 120, 300); God = $true }
}
Push-Location $Root
try {
    $failed = 0
    foreach ($name in $Case) {
        $c = $cases[$name]
        if (-not $c) { "unknown case $name"; $failed++; continue }
        $file = "states\$($c.State).ps2state"
        if (-not (Test-Path $file)) { "${name}: no save state $file"; $failed++; continue }
        $start = [int]((python tools\state_dump.py $file | Select-String "vblank tick\s+(\d+)").Matches[0].Groups[1].Value)
        $ticks = @($c.Offsets | ForEach-Object { $t = $start + $_; if ($t % 2 -ne 0) { $t++ }; $t })
        $shift = {
            param($list, $separator)
            @(foreach ($item in ($list -split $separator | Where-Object { $_ })) {
                $at, $rest = $item.Split(":", 2)
                "$($start + [int]$at):$rest"
            }) -join $separator
        }
        $names = @("TF_INPUT", "TF_GOTO", "TF_GOD", "PS2X_TURBO_VSYNCS", "TF_QUIT_VSYNC", "PS2X_SHOT_VSYNCS", "PS2X_GPU_MIRROR",
                   "PS2X_GPU_SCALE", "PS2X_TEXTURE_PACK", "TF_60FPS", "TF_DRAW_DISTANCE", "PS2X_GPU_NATIVE",
                   "PS2X_GPU_FULL_FIELDS") + @($Set.Keys)
        foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
        $env:TF_INPUT = if ($c.Do) { & $shift $c.Do "," } else { "" }
        if ($c.Goto) { $env:TF_GOTO = & $shift $c.Goto ";" }
        if ($c.God) { $env:TF_GOD = "1" }
        $env:PS2X_TURBO_VSYNCS = "100000000"
        $env:TF_QUIT_VSYNC = "$(($ticks | Measure-Object -Maximum).Maximum + 10)"
        $env:PS2X_SHOT_VSYNCS = ($ticks -join ",")
        # The kept frames are the software renderer's: it draws, and the GPU beside it, with the
        # game's own textures (a texture pack in replacements\ would change the GPU's frames).
        $env:PS2X_GPU_MIRROR = "1"
        $env:PS2X_GPU_SCALE = "1"
        $env:PS2X_TEXTURE_PACK = "0"
        # The exact pictures: the emulated pixel pipeline and fields at half the scale down (the native
        # path and full fields are the game's defaults since 2026-10-08; check_native.ps1 compares them).
        $env:PS2X_GPU_NATIVE = "0"
        $env:PS2X_GPU_FULL_FIELDS = "0"
        $env:TF_60FPS = "0" # (the kept frames are of the console's 30 frames a second)
        $env:TF_DRAW_DISTANCE = "1" # (and of its draw distance)
        foreach ($key in $Set.Keys) { Set-Item "Env:$key" $Set[$key] }
        $runArgs = @{ Seconds = 600; Name = "check_$name"; LoadState = $c.State }
        if ($Exe) { $runArgs.Exe = $Exe }
        .\run.ps1 @runArgs *> "runs\check_$name.console.txt"
        foreach ($n in $names) { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }

        $golden = Join-Path $GoldenRoot $name
        if ($Update) {
            New-Item -ItemType Directory -Force $golden | Out-Null
            $kept = 0
            foreach ($t in $ticks) { if (Test-Path "runs\shot_$t.png") { Copy-Item "runs\shot_$t.png" $golden -Force; $kept++ } }
            # The GPU's frames too, for an exact check of a change to the GPU renderer.
            foreach ($t in $ticks) { if (Test-Path "runs\shot_${t}_gpu.png") { Copy-Item "runs\shot_${t}_gpu.png" $golden -Force } }
            "${name}: kept $kept of $($ticks.Count) frames in $golden"
            continue
        }
        $same = 0
        foreach ($t in $ticks) {
            $shot = "runs\shot_$t.png"
            $kept = Join-Path $golden "shot_$t.png"
            if (-not (Test-Path $shot)) { "${name}: tick ${t}: no frame"; continue }
            if (-not (Test-Path $kept)) { "${name}: tick ${t}: nothing kept to compare with (-Update)"; continue }
            if ((Get-FileHash $kept).Hash -eq (Get-FileHash $shot).Hash) { $same++ }
            else { "${name}: tick ${t}: DIFFERENT (python tools\frame_diff.py $kept $shot)" }
        }
        "${name}: $same of $($ticks.Count) frames identical"
        if ($same -ne $ticks.Count) { $failed++ }
        # The GPU's frames against the GPU's kept ones, where the set has them.
        $gpuKept = @($ticks | Where-Object { Test-Path (Join-Path $golden "shot_${_}_gpu.png") })
        if ($gpuKept.Count -gt 0) {
            $gpuSame = 0
            foreach ($t in $gpuKept) {
                $shot = "runs\shot_${t}_gpu.png"
                $kept = Join-Path $golden "shot_${t}_gpu.png"
                if ((Test-Path $shot) -and (Get-FileHash $kept).Hash -eq (Get-FileHash $shot).Hash) { $gpuSame++ }
                else { "${name}: tick ${t}: GPU frame DIFFERENT (python tools\frame_diff.py $kept $shot)" }
            }
            "${name}: $gpuSame of $($gpuKept.Count) GPU frames identical"
            if ($gpuSame -ne $gpuKept.Count) { $failed++ }
        }
        # The GPU's frames next to the software renderer's.
        $report = @(python tools\gpu_compare.py runs 2>$null)
        $worst = $null
        foreach ($t in $ticks) {
            $line = $report | Select-String "^tick ${t}: mean difference ([\d.]+)"
            if ($line) { $value = [double]$line.Matches[0].Groups[1].Value; if ($null -eq $worst -or $value -gt $worst) { $worst = $value } }
        }
        if ($null -ne $worst) { "${name}: the GPU's frames against these: mean difference at most $worst per channel" }
    }
    if (-not $Update) { if ($failed) { "FAILED: $failed case(s)" } else { "all cases identical" } }
}
finally { Pop-Location }
