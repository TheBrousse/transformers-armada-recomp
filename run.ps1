<#
.SYNOPSIS
    Runs TransformersRecomp for a limited time and saves its output (debugging aid).

.EXAMPLE
    .\run.ps1 -Seconds 30 -Name boot     # writes runs\boot.out / runs\boot.err
.EXAMPLE
    .\run.ps1 -Seconds 120 -Name test -ScratchCard   # empty memory card in runs\card_test
.EXAMPLE
    .\run.ps1 -Seconds 0 -Monitor 0 -Scale auto -Fullscreen -Sound   # play: full screen, the screen's resolution
.EXAMPLE
    .\run.ps1 -Seconds 0 -LoadState a11 -Scale 3 -Sound   # from a state, at 3x the PS2's resolution
.EXAMPLE
    .\run.ps1 -Seconds 0 -Monitor 0 -Scale auto -Fullscreen -Widescreen -Sound   # play in 16:9
.EXAMPLE
    .\run.ps1 -Seconds 0 -Monitor 0 -Scale auto -Fullscreen -Widescreen -ShowFps -Sound   # ... with the FPS counter
.EXAMPLE
    .\run.ps1 -Seconds 0 -Monitor 0 -Scale auto -Fullscreen -Widescreen -FpsLock 120 -ShowFps -Sound   # ... locked at 120 pictures a second (or 60; F4 switches)
.EXAMPLE
    .\run.ps1 -Seconds 0 -Monitor 0 -Scale auto -Fullscreen -Widescreen -Fps30 -ShowFps -Sound   # ... at the console's 30 frames a second (levels run 60 by default)
.EXAMPLE
    .\run.ps1 -Seconds 0 -Monitor 0 -Scale auto -Fullscreen -Widescreen -DrawDistance 3 -Sound   # the scene's detail reaching 3x as far (2x by default, 1 = the console's)
.EXAMPLE
    .\run.ps1 -Seconds 120 -Name make -ScratchCard -SaveState dropzones -SaveAt 5700
    .\run.ps1 -Seconds 60 -Name world -LoadState dropzones   # starts where the state was made
#>
param(
    [int]$Seconds = 30,
    [string]$Name = "run",
    [string]$Config = "RelWithDebInfo",
    [string]$Exe = "",            # another build to run (e.g. out\play\TransformersRecomp.exe), else out\build's
    [int]$Monitor = 1,            # window placement (TF_MONITOR); -1 = leave it to the OS
    [switch]$ScratchCard,         # start from an empty memory card (TF_MC_ROOT), keeping real saves out of tests
    [string]$CardFrom,            # ... or from a copy of this card folder (e.g. runs\card_<earlier run>)
    [string]$LoadState,           # start from states\<name>.ps2state instead of booting the game
    [string]$SaveState,           # write states\<name>.ps2state ...
    [int]$SaveAt = 0,             # ... at this VBlank tick (TF_FPS prints the tick)
    [switch]$Sound,               # play the game's sound (test runs are silent otherwise)
    [switch]$DumpSound,           # write what the game plays to runs\<name>.wav
    [switch]$Gpu,                 # draw with the GPU renderer (the default; PS2X_GPU=1)
    [switch]$Software,            # draw with the software renderer instead (PS2X_GPU=0)
    [string]$Scale = "",          # GPU: this many times the PS2's resolution, "<x>x<y>", or "auto" (PS2X_GPU_SCALE; runs use 1 unless set)
    [switch]$Fullscreen,          # borderless full screen (PS2X_FULLSCREEN; F11 switches)
    [switch]$Widescreen,          # the game's 16:9 mode (PS2X_WIDESCREEN), the game's default; runs are 4:3 unless asked
    [switch]$Fps60,               # 60 frames a second in levels (the default; TF_60FPS=1)
    [switch]$Fps30,               # the console's 30 frames a second (TF_60FPS=0)
    [int]$FrameRate = 0,          # levels at this many frames a second: 60 (the default), 120, 144, ... (TF_FRAME_RATE)
    [string]$FpsLock = "",        # pictures a second in levels: refresh (the screen's: the game's default; runs use 120 unless -Settings), 120, 60, or 0 free (TF_FPS_LOCK; F4 switches 120/60 while playing)
    [string]$DrawDistance = "",   # how far the scene's detail reaches, times this (TF_DRAW_DISTANCE; 2 by default, 1 = the console's)
    [switch]$ShowFps,             # the FPS counter in the window's corner (PS2X_SHOW_FPS; F3 switches it)
    [switch]$Settings             # the settings panel's choices (settings.ini, F5 in the game); runs leave them out
)

$Root = $PSScriptRoot
$exe = if ($Exe) { (Resolve-Path $Exe).Path } else { Join-Path $Root "out\build\transformers\$Config\TransformersRecomp.exe" }
$runs = Join-Path $Root "runs"
$states = Join-Path $Root "states"
New-Item -ItemType Directory -Force $runs | Out-Null
$out = Join-Path $runs "$Name.out"
$err = Join-Path $runs "$Name.err"

# Open the window on another monitor, unfocused, so runs stay out of the way.
if ($Monitor -ge 0) { $env:TF_MONITOR = "$Monitor" }
# Its console too: on the second monitor, beside the window (tools\console_window.ps1).
. (Join-Path $Root "tools\console_window.ps1")

# The settings panel's file (settings.ini, written by F5 in the game): only with -Settings, so that
# test runs are the same whatever was chosen while playing.
$callerSettingsFile = $env:PS2X_SETTINGS_FILE # (given back once the game is started; -Settings uses it)
if (-not $Settings) { $env:PS2X_SETTINGS_FILE = "0" }

# Test runs are silent unless asked; the sound can be written to a file either way.
if ($Sound -or $Settings) { Remove-Item Env:PS2X_AUDIO -ErrorAction SilentlyContinue } else { $env:PS2X_AUDIO = "0" }
if ($DumpSound) { $env:PS2X_AUDIO_DUMP = Join-Path $runs "$Name.wav" } else { Remove-Item Env:PS2X_AUDIO_DUMP -ErrorAction SilentlyContinue }

# The renderer: the GPU's unless -Software. Left as the caller set it when no switch is given,
# so bench.ps1 -Set can choose. Runs draw at the PS2's resolution unless a scale is asked for
# (the game alone picks one from the screen), so their pictures and timings stay comparable.
$callerGpu = $env:PS2X_GPU         # (both given back once the game is started: a later start
$callerScale = $env:PS2X_GPU_SCALE #  from this shell decides for itself)
if ($Gpu) { $env:PS2X_GPU = "1" }
if ($Software) { $env:PS2X_GPU = "0" }
# (-Settings: as the game starts on its own - the settings file, else the game's defaults.)
if ($Scale) { $env:PS2X_GPU_SCALE = $Scale } elseif (-not $env:PS2X_GPU_SCALE -and -not $Settings) { $env:PS2X_GPU_SCALE = "1" }
if ($Fullscreen) { $env:PS2X_FULLSCREEN = "1" } else { Remove-Item Env:PS2X_FULLSCREEN -ErrorAction SilentlyContinue }
$callerWidescreen = $env:PS2X_WIDESCREEN # (given back too)
if ($Widescreen) { $env:PS2X_WIDESCREEN = "1" } elseif (-not $env:PS2X_WIDESCREEN -and -not $Settings) { $env:PS2X_WIDESCREEN = "0" }
$callerFps60 = $env:TF_60FPS # (given back too; a save state made at 30 keeps 30: docs\DEBUGGING.md)
if ($Fps60) { $env:TF_60FPS = "1" }
if ($Fps30) { $env:TF_60FPS = "0" }
$callerFrameRate = $env:TF_FRAME_RATE # (given back too)
if ($FrameRate -gt 0) { $env:TF_FRAME_RATE = "$FrameRate" }
$callerFpsLock = $env:TF_FPS_LOCK # (given back too)
if ($FpsLock) { $env:TF_FPS_LOCK = $FpsLock }
# Test runs keep what the game did before the defaults of 2026-10-08 - 120 pictures a second, no
# multisampling, no texture cache - so that their pictures and timings stay comparable with the
# kept sets and earlier numbers (-Settings, or the variables set: as asked).
$callerMsaa = $env:PS2X_GPU_MSAA; $callerTextureCache = $env:PS2X_GPU_TEXTURE_CACHE # (given back too)
if (-not $Settings) {
    if (-not $env:TF_FPS_LOCK -and -not $env:TF_FRAME_RATE -and $FrameRate -le 0) { $env:TF_FPS_LOCK = "120" }
    if (-not $env:PS2X_GPU_MSAA) { $env:PS2X_GPU_MSAA = "0" }
    if (-not $env:PS2X_GPU_TEXTURE_CACHE) { $env:PS2X_GPU_TEXTURE_CACHE = "0" }
}
$callerDrawDistance = $env:TF_DRAW_DISTANCE # (given back too)
if ($DrawDistance) { $env:TF_DRAW_DISTANCE = $DrawDistance }
if ($ShowFps) { $env:PS2X_SHOW_FPS = "1" } else { Remove-Item Env:PS2X_SHOW_FPS -ErrorAction SilentlyContinue }

# Save states (F6 saves and F9 loads states\quick.ps2state in the window). A state carries
# the memory card it was made with, which replaces the card folder on load - so a run
# that loads one always uses a scratch card.
foreach ($variable in "PS2X_LOAD_STATE", "PS2X_SAVE_STATE", "PS2X_SAVE_STATE_TICK", "PS2X_STATE_RESTORE_CARD") {
    Remove-Item "Env:$variable" -ErrorAction SilentlyContinue
}
$env:PS2X_STATE_FILE = Join-Path $states "quick.ps2state"
if ($LoadState) {
    $file = Join-Path $states "$LoadState.ps2state"
    if (-not (Test-Path $file)) { throw "No save state $file. Make it with -SaveState $LoadState -SaveAt <tick>." }
    $env:PS2X_LOAD_STATE = $file
    $env:PS2X_STATE_RESTORE_CARD = "1"
    $ScratchCard = $true
}
if ($SaveState) {
    if ($SaveAt -le 0) { throw "-SaveState needs -SaveAt <VBlank tick>." }
    New-Item -ItemType Directory -Force $states | Out-Null
    $env:PS2X_SAVE_STATE = Join-Path $states "$SaveState.ps2state"
    $env:PS2X_SAVE_STATE_TICK = "$SaveAt"
}

if ($CardFrom) { $ScratchCard = $true }
if ($ScratchCard) {
    $card = Join-Path $runs "card_$Name"
    if (Test-Path $card) { Remove-Item -Recurse -Force $card }
    New-Item -ItemType Directory -Force $card | Out-Null
    if ($CardFrom) {
        if (-not (Test-Path $CardFrom)) { throw "no card folder $CardFrom" }
        Copy-Item -Recurse -Path (Join-Path $CardFrom "*") -Destination $card
    }
    $env:TF_MC_ROOT = $card
} else {
    Remove-Item Env:TF_MC_ROOT -ErrorAction SilentlyContinue
}
$consolesBefore = Get-ConsoleWindows $exe
$started = Get-Date
$p = Start-Process -FilePath $exe -WorkingDirectory $Root -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -WindowStyle Hidden
if ($callerScale) { $env:PS2X_GPU_SCALE = $callerScale } else { Remove-Item Env:PS2X_GPU_SCALE -ErrorAction SilentlyContinue }
if ($callerGpu) { $env:PS2X_GPU = $callerGpu } else { Remove-Item Env:PS2X_GPU -ErrorAction SilentlyContinue }
Remove-Item Env:PS2X_FULLSCREEN -ErrorAction SilentlyContinue
if ($callerWidescreen) { $env:PS2X_WIDESCREEN = $callerWidescreen } else { Remove-Item Env:PS2X_WIDESCREEN -ErrorAction SilentlyContinue }
if ($callerFps60) { $env:TF_60FPS = $callerFps60 } else { Remove-Item Env:TF_60FPS -ErrorAction SilentlyContinue }
if ($callerDrawDistance) { $env:TF_DRAW_DISTANCE = $callerDrawDistance } else { Remove-Item Env:TF_DRAW_DISTANCE -ErrorAction SilentlyContinue }
if ($callerFrameRate) { $env:TF_FRAME_RATE = $callerFrameRate } else { Remove-Item Env:TF_FRAME_RATE -ErrorAction SilentlyContinue }
if ($callerFpsLock) { $env:TF_FPS_LOCK = $callerFpsLock } else { Remove-Item Env:TF_FPS_LOCK -ErrorAction SilentlyContinue }
if ($callerMsaa) { $env:PS2X_GPU_MSAA = $callerMsaa } else { Remove-Item Env:PS2X_GPU_MSAA -ErrorAction SilentlyContinue }
if ($callerTextureCache) { $env:PS2X_GPU_TEXTURE_CACHE = $callerTextureCache } else { Remove-Item Env:PS2X_GPU_TEXTURE_CACHE -ErrorAction SilentlyContinue }
if ($callerSettingsFile) { $env:PS2X_SETTINGS_FILE = $callerSettingsFile } else { Remove-Item Env:PS2X_SETTINGS_FILE -ErrorAction SilentlyContinue }
Show-GameConsole $p $exe $consolesBefore
$left = [Math]::Max(1, $Seconds * 1000 - [int]((Get-Date) - $started).TotalMilliseconds)
if ($Seconds -gt 0 -and -not $p.WaitForExit($left)) {
    Stop-Process -Id $p.Id -Force
    Write-Host "stopped after $Seconds s"
} else {
    $p.WaitForExit()
    Write-Host "exited with code $($p.ExitCode)"
}
Write-Host "logs: $out, $err"
