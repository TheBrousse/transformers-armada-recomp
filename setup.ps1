<#
.SYNOPSIS
    Sets up the Transformers PC version from your own disc: one command. Run it again any time
    (finished steps are kept, so an interrupted setup carries on).

.DESCRIPTION
    1. Checks what the build needs: Git, Python 3.11+ (with Pillow and numpy), CMake, Visual Studio's
       C++ tools. -InstallPrerequisites installs the missing ones with winget (Visual Studio Build
       Tools is a few GB); without it the script says what to install and stops.
    2. Builds the game (build.ps1): checks the disc is Transformers USA (SLUS-20668), extracts it into
       game\, recompiles the game's code and builds the exe. The first build takes 15-25 minutes.
    3. Makes the HD textures (tools\make_hd_textures.ps1): every texture upscaled on your GPU, tens of
       minutes to a few hours. -SkipHdTextures leaves the upscale out and makes only the sharp fonts
       and button prompts (the game then shows its original textures).
    4. Remembers your disc path in .setup.json (git-ignored) for update.ps1.

    Everything made from your disc stays on your PC: the folders it writes are git-ignored. Never
    commit or share them.

    -Root and -Hooks are for a repository that holds this folder as a subfolder and adds steps of
    its own (it passes its top folder and a script with Invoke-SetupHook / Invoke-UpdateHook).

.EXAMPLE
    .\setup.ps1 -Iso "D:\Games\Transformers (USA).iso" -InstallPrerequisites
.EXAMPLE
    .\setup.ps1          # again later: reuses the path it remembered
#>
param(
    [string]$Iso,
    [switch]$SkipHdTextures,
    [switch]$InstallPrerequisites,
    [string]$Root,                 # the git repository's top (default: this folder)
    [string]$Hooks,                # a script with extra steps (Invoke-SetupHook), dot-sourced
    [hashtable]$HookArgs = @{}     # passed to the hooks as $ctx.Args
)

$ErrorActionPreference = "Stop"
$Port = $PSScriptRoot
if (-not $Root) { $Root = $Port }
$Root = (Resolve-Path $Root).Path
$StateFile = Join-Path $Root ".setup.json"

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "`nsetup: $msg" -ForegroundColor Red; exit 1 }

# The shared helpers: real-Python detection (not Windows' Store shortcut), probes, git long paths.
. (Join-Path $Port "tools\setup_common.ps1")
if ($Hooks) { . $Hooks }

# --- the remembered choices ---------------------------------------------------------------------
$state = if (Test-Path $StateFile) { Get-Content $StateFile -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
if (-not $state.PSObject.Properties["Iso"]) { $state | Add-Member Iso "" }
if ($Iso) {
    if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) { Fail "no such file: $Iso" }
    $state.Iso = (Resolve-Path -LiteralPath $Iso).Path
}
# (written now: the build reads it - an install's version knows it was set up, transformers\version.cmake)
$state | ConvertTo-Json | Set-Content $StateFile -Encoding UTF8

$gameDir = Join-Path $Port "game"
$haveGame = Test-Path (Join-Path $gameDir "SLUS_206.68")
if (-not $haveGame -and -not $state.Iso) {
    Fail "first setup: give your disc image, e.g.  .\setup.ps1 -Iso `"D:\Games\Transformers (USA).iso`""
}
# The disc, checked before the long installs: only Transformers (USA), SLUS-20668, works. (The build
# checks the game's executable inside it; a size other than the USA release's is only a warning.)
if (-not $haveGame) {
    if (-not (Test-Path -LiteralPath $state.Iso -PathType Leaf)) { Fail "the disc image is gone: $($state.Iso) (give it again with -Iso)" }
    $size = (Get-Item -LiteralPath $state.Iso).Length
    if ($size -ne (Get-UsaDiscSize)) {
        Write-Host "warning: $($state.Iso) is not the size of the USA release (SLUS-20668, $(Get-UsaDiscSize) bytes); the build checks the disc's executable." -ForegroundColor Yellow
    }
}

# --- 1. what the build needs --------------------------------------------------------------------
Step "Checking what the build needs"
$missing = @(Get-MissingPrerequisites)
if ($missing.Count -gt 0) {
    $how = Get-PrerequisiteInstall
    if (-not $InstallPrerequisites) {
        $lines = $missing | ForEach-Object { "  - $($how[$_][0]):  $($how[$_][1])" }
        Fail ("missing:`n" + ($lines -join "`n") + "`nInstall them (or run setup.ps1 -InstallPrerequisites), open a NEW PowerShell, run setup.ps1 again.")
    }
    foreach ($m in $missing) {
        Step "Installing $($how[$m][0])"
        Invoke-Native { & cmd /c $how[$m][1] } | Out-Null   # (its exit code: the check after it decides)
    }
    # (what was just installed, found without a new PowerShell)
    Update-PathFromRegistry
    $still = @(Get-MissingPrerequisites)
    if ($still.Count -gt 0) {
        Fail ("prerequisites installed, but not found yet: $($still -join ', '). Open a NEW PowerShell (or restart the PC after the Visual Studio install) and run setup.ps1 again.")
    }
}
$Python = Find-Python
Use-Python $Python
Write-Host "Python: $Python"
foreach ($pkg in @(@("PIL", "pillow", "Pillow (Python imaging)"), @("numpy", "numpy", "numpy (Python): the sharp text and button symbols"))) {
    Invoke-Probe { & $Python -c "import $($pkg[0])" } | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Step "Installing $($pkg[2])"
        if ((Invoke-Native { & $Python -m pip install --user $pkg[1] }) -ne 0) { Fail "pip install $($pkg[1]) failed" }
    }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Step "Installing CMake (pip)"
    if ((Invoke-Native { & $Python -m pip install --user cmake }) -ne 0) { Fail "pip install cmake failed" }
    Use-Python $Python   # (its user Scripts folder, where cmake.exe went, now exists)
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail "cmake installed but not found on PATH" }
}

# The check that refuses commits with game data (tools\check_no_game_data.py), for anyone who commits.
if (Test-Path (Join-Path $Root ".githooks")) { Invoke-Native { & git -C $Root config core.hooksPath .githooks } | Out-Null }

# --- 2. the game ----------------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $Port "PS2Recomp\CMakeLists.txt"))) {
    Step "Fetching the PS2Recomp submodule"
    if ((Invoke-Native { & git -C $Root submodule update --init --recursive }) -ne 0) { Fail "git submodule update failed" }
}
Step "Building the game (build.ps1)"
$buildArgs = @{}
if (-not $haveGame) { $buildArgs.Iso = $state.Iso }
& (Join-Path $Port "build.ps1") @buildArgs
if (-not $?) { Fail "the build failed (see above)" }

# --- 3. the HD textures ----------------------------------------------------------------------------
if ($SkipHdTextures) {
    Step "Making the button prompts and fonts (no GPU upscale)"
    Invoke-OptionalFonts -Python $Python -Port $Port
} else {
    Step "Making the HD textures (tools\make_hd_textures.ps1)"
    & (Join-Path $Port "tools\make_hd_textures.ps1") -Python $Python
    if (-not $?) { Fail "making the HD textures failed (see above); -SkipHdTextures plays without them" }
}

# --- the steps of a repository that holds this folder (its hooks) ---------------------------------
if (Get-Command Invoke-SetupHook -ErrorAction SilentlyContinue) {
    $ctx = New-HookContext -Root $Root -Port $Port -Python $Python -State $state -HookArgs $HookArgs
    try { Invoke-SetupHook $ctx } catch { Fail $_.Exception.Message }
}

$state | Add-Member -Force SetUpAt (& git -C $Root rev-parse HEAD)
$state | ConvertTo-Json | Set-Content $StateFile -Encoding UTF8
Write-Host "`nReady. Play with Play.bat in $Root. Settings: F5 in the game." -ForegroundColor Green
