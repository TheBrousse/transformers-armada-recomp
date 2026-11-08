<#
.SYNOPSIS
    Plays a stretch of the game from a save state with scripted input, and keeps what it
    showed: a contact sheet of frames, the log, optionally a new save state and the sound.

.DESCRIPTION
    A way to play through the game step by step without sitting at it: every step starts
    where the last one ended. Ticks in -Do count from the start of the step (60 per second
    of game time); the buttons and sticks are those of TF_INPUT (docs\DEBUGGING.md).

.EXAMPLE
    .\tools\play.ps1 -From amazon -To a1 -Ticks 300 -Do "0:lup:240,240:rright@50:40"
    .\tools\play.ps1 -From a1 -To a2 -Ticks 600 -Do "0:lup:600" -Shots 10 -Sound
    .\tools\play.ps1 -From a2 -To a3 -Ticks 900 -Goto "60:760,-1667;500:682,-1789,8" -Do "30:cross"
#>
param(
    [string]$From = "amazon",     # state to start from (states\<name>.ps2state)
    [string]$To,                  # state to write at the end
    [string]$Do = "",             # input, in ticks from the start: "0:lup:300,300:r1:20"
    [string]$Goto = "",           # places to walk to (TF_GOTO), ticks from the start: "0:760,-1667;600:682,-1789,8"
    [string]$Warp = "",           # places to put the player at (TF_WARP), ticks from the start: "30:760,-1667,50"
    [int]$Ticks = 300,            # how long to play
    [int]$Shots = 6,              # frames to keep, evenly spaced, the last one at the end
    [string]$Name = "play",
    [string]$Peek = "",           # TF_PEEK list, to log memory along the way
    [switch]$Sound,               # dump the sound to runs\<name>.wav
    [switch]$Trace,               # the sound driver's own trace (tools\host0\SIRENS3.INI)
    [switch]$God,                 # the game's own god mode (TF_GOD): energy never runs out
    [switch]$RealTime,            # at the game's speed instead of as fast as possible
    [int]$Seconds = 0             # wall-clock limit (default: generous for the tick count)
)
$Root = Split-Path $PSScriptRoot -Parent
Push-Location $Root
try {
    $state = "states\$From.ps2state"
    if (-not (Test-Path $state)) { throw "no save state $state" }
    $start = [int]((python tools\state_dump.py $state | Select-String "vblank tick\s+(\d+)").Matches[0].Groups[1].Value)
    $end = $start + $Ticks
    if ($end % 2 -ne 0) { $end++ }        # the same field every time

    $script = @()
    foreach ($item in ($Do -split "," | Where-Object { $_ })) {
        $at, $rest = $item.Split(":", 2)
        $script += "$($start + [int]$at):$rest"
    }
    $walk = @()
    foreach ($item in ($Goto -split ";" | Where-Object { $_ })) {
        $at, $rest = $item.Split(":", 2)
        $walk += "$($start + [int]$at):$rest"
    }
    $warps = @()
    foreach ($item in ($Warp -split ";" | Where-Object { $_ })) {
        $at, $rest = $item.Split(":", 2)
        $warps += "$($start + [int]$at):$rest"
    }
    $shotTicks = @()
    for ($i = 1; $i -le $Shots; $i++) {
        $t = $start + [int]($Ticks * $i / $Shots)
        if ($t % 2 -ne 0) { $t++ }
        $shotTicks += $t
    }
    $shotTicks = $shotTicks | Select-Object -Unique

    $env:TF_INPUT = $script -join ","
    if ($walk) { $env:TF_GOTO = $walk -join ";" } else { Remove-Item Env:TF_GOTO -ErrorAction SilentlyContinue }
    if ($warps) { $env:TF_WARP = $warps -join ";" } else { Remove-Item Env:TF_WARP -ErrorAction SilentlyContinue }
    if ($God) { $env:TF_GOD = "1" } else { Remove-Item Env:TF_GOD -ErrorAction SilentlyContinue }
    $env:PS2X_TURBO_VSYNCS = if ($RealTime) { "0" } else { "100000000" }
    $env:PS2X_SHOT_VSYNCS = $shotTicks -join ","
    $env:TF_QUIT_VSYNC = "$($end + 12)"
    if ($Peek) { $env:TF_PEEK = $Peek } else { Remove-Item Env:TF_PEEK -ErrorAction SilentlyContinue }
    if ($Trace) { $env:TF_HOST_ROOT = "tools\host0" } else { Remove-Item Env:TF_HOST_ROOT -ErrorAction SilentlyContinue }
    if ($Seconds -le 0) { $Seconds = 60 + [int]($Ticks / 20) }

    $arguments = @{ Seconds = $Seconds; Name = $Name; LoadState = $From }
    if ($To) { $arguments.SaveState = $To; $arguments.SaveAt = $end }
    if ($Sound) { $arguments.DumpSound = $true }
    .\run.ps1 @arguments *> "runs\$Name.console.txt"
    Remove-Item Env:TF_PEEK, Env:TF_HOST_ROOT, Env:TF_GOTO, Env:TF_WARP, Env:TF_GOD -ErrorAction SilentlyContinue

    $frames = "runs\$Name"
    New-Item -ItemType Directory -Force $frames | Out-Null
    Remove-Item "$frames\*.png" -ErrorAction SilentlyContinue
    foreach ($t in $shotTicks) {
        if (Test-Path "runs\shot_$t.png") { Move-Item "runs\shot_$t.png" "$frames\t$t.png" -Force }
    }
    $kept = @(Get-ChildItem "$frames\*.png").Count
    if ($kept -gt 0) {
        $columns = [Math]::Min($kept, 4)
        python tools\contact_sheet.py $frames "runs\${Name}_sheet.png" --cols $columns --width 320 | Out-Null
    }
    "ticks $start..$end, $kept frames in runs\${Name}_sheet.png" + $(if ($To) { ", state $To" } else { "" })
    # What the game and the runtime said that is worth a look.
    Get-Content "runs\$Name.err" | Where-Object {
        $_ -match "\[tf\]|\[tf:goto\]|\[tf:warp\]|\[state\]|error|warning|Open\(|Tone '|not found|did not return|watchdog|fatal" -and
        $_ -notmatch "modload"
    } | ForEach-Object { if ($_.Length -gt 180) { $_.Substring(0, 180) } else { $_ } } | Select-Object -First 40
}
finally { Pop-Location }
