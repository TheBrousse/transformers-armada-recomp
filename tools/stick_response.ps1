<#
.SYNOPSIS
    Measures how the game answers the right stick: holds it to the right at a given
    deflection for a given time and reports how far the view turned.

.DESCRIPTION
    Runs from the `amazon` save state in turbo mode (repeatable). Each case is
    "<percent>:<ticks>": the stick is held at that deflection from tick 6660 for that many
    VBlank ticks, and the frame shown afterwards is compared with the one from a run without
    input (tools\frame_shift.py). The smallest deflection that turns the view at all is the
    game's dead zone; the shifts over deflection and time show the shape of the response.

.EXAMPLE
    .\tools\stick_response.ps1 -Cases "10:20","20:20","40:20","100:8","100:16"
#>
param(
    [string[]]$Cases = @("10:20", "20:20", "30:20", "40:20", "60:20", "100:20"),
    [string]$Stick = "rright",
    [int]$MaxShift = 300,
    [string]$State = "amazon"
)
$Root = Split-Path $PSScriptRoot -Parent
Push-Location $Root
try {
    $out = "runs\stick_response"
    New-Item -ItemType Directory -Force $out | Out-Null
    $env:PS2X_TURBO_VSYNCS = "13000"
    $reference = @{}
    foreach ($case in $Cases) {
        $percent, $hold = $case.Split(":")
        $shot = 6660 + [int]$hold + 10
        if ($shot % 2 -ne 0) { $shot++ }      # always the same field
        $env:PS2X_SHOT_VSYNCS = "$shot"
        $env:TF_QUIT_VSYNC = "$($shot + 10)"
        if (-not $reference.ContainsKey($shot)) {
            $env:TF_INPUT = ""
            .\run.ps1 -Seconds 120 -Name stick_response -LoadState $State *> runs\stick_response.console.txt
            Copy-Item "runs\shot_$shot.png" "$out\none_$shot.png" -Force
            $reference[$shot] = "$out\none_$shot.png"
        }
        $env:TF_INPUT = "6660:$Stick@${percent}:$hold"
        .\run.ps1 -Seconds 120 -Name stick_response -LoadState $State *> runs\stick_response.console.txt
        $frame = "$out\p${percent}_h$hold.png"
        Copy-Item "runs\shot_$shot.png" $frame -Force
        $result = python tools\frame_shift.py $reference[$shot] $frame --max $MaxShift
        "{0,4}% for {1,3} ticks:  {2}" -f $percent, $hold, $result
    }
}
finally { Pop-Location }
