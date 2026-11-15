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
.EXAMPLE
    .\tools\make_hd_textures.ps1 -Verify         # (TODO 281) the set checked and reported, nothing changed
.EXAMPLE
    .\tools\make_hd_textures.ps1 -Remake         # (TODO 281) the last report's broken images made again
#>
param([switch]$SkipUpscale, [switch]$CrispFontsOnly, [switch]$FontsOnly, [string]$Python = "python",
      [ValidateSet("", "fast", "balanced", "best")][string]$Quality = "",
      [switch]$Resume, [int]$GamePid = 0, [switch]$Verify, [switch]$Remake,
      [switch]$RetryResets,  # (TODO 382) F1 > Make HD textures: tried again on a card whose driver resets stopped the last runs
      [switch]$Gentle)  # (TODO 310) easy on the graphics card (or "gentle": true in hd_options.json): rests, about three times as slow

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
# (TODO 279: the disc's files may be in the install's data\game - install_layout.ps1)
. (Join-Path $PSScriptRoot "install_layout.ps1")
# (TODO 310) Each step and batch to TF_STEP_LOG with its time, when the setup or the update set it (step_log.ps1)
. (Join-Path $PSScriptRoot "step_log.ps1")
$Game = Get-GameDataDir $Root
$Out = Join-Path $Root "hd_textures"
$Py = Join-Path $PSScriptRoot "hd_textures"

# (TODO 219) Started by the game (-Resume): an HD set left unfinished is carried on in a small window of its own,
# which waits while the game runs and works once it closes. Python is looked for as the setup does.
# (TODO 281) F1's "Verify HD textures" (-Verify: the set checked and reported, nothing changed) and "Remake N broken
# images" (-Remake: the images that verify found missing or broken removed, then made again as -Resume makes them).
if ($Remake) { $Resume = $true }
if ($Verify) {
    try { $Host.UI.RawUI.WindowTitle = "Verifying the HD textures (nothing is changed)" } catch {}
    . (Join-Path $PSScriptRoot "setup_common.ps1")
    if ($Python -eq "python") { $found = Find-Python; if ($found) { $Python = $found } }
    Write-Host "Checking every HD texture. Nothing is changed; the result shows in F1 > Graphics."
}
if ($Resume) {
    try { $Host.UI.RawUI.WindowTitle = "Making the HD textures (you can close this; it carries on next time)" } catch {}
    . (Join-Path $PSScriptRoot "setup_common.ps1")
    if ($Python -eq "python") { $found = Find-Python; if ($found) { $Python = $found } }
    if ($GamePid -gt 0) {
        # (TODO 267, the user's choice: the game's own window makes them only while the game is open - beside it,
        # gently; it stops when the game closes (the game ends it), and the next start carries on)
        Write-Host "Making the rest of the HD textures while the game is open. Closing the game stops this;"
        Write-Host "what is made is kept and the next start of the game carries on."
    }
    else {
        Write-Host "Making the rest of the HD textures. It waits while the game runs and carries on when you close it."
        Write-Host "You can close this window at any time: it carries on the next time you start the game."
    }
}
# (TODO 382) In a console window of its own (the game's small window), QuickEdit off: with it, a click in the window
# selects text and Windows holds the program at its next line of output until a key is pressed - it looks stuck. Only
# when this script is alone in its console (never a terminal the player opened himself, which keeps its own setting).
try {
    Add-Type -Namespace TfHd -Name Con -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern System.IntPtr GetConsoleWindow();
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int n);
[DllImport("kernel32.dll")] public static extern bool GetConsoleMode(System.IntPtr h, out uint m);
[DllImport("kernel32.dll")] public static extern bool SetConsoleMode(System.IntPtr h, uint m);
[DllImport("kernel32.dll")] public static extern uint GetConsoleProcessList(uint[] list, uint count);
'@ -ErrorAction Stop
    if ([TfHd.Con]::GetConsoleWindow() -ne [IntPtr]::Zero -and [TfHd.Con]::GetConsoleProcessList((New-Object uint32[] 8), 8) -eq 1) {
        $h = [TfHd.Con]::GetStdHandle(-10)   # (the console's input)
        $m = 0
        if ([TfHd.Con]::GetConsoleMode($h, [ref]$m) -and ($m -band 0x40)) {
            [void][TfHd.Con]::SetConsoleMode($h, (($m -band (-bnot 0x40)) -bor 0x80))   # (QuickEdit off, ENABLE_EXTENDED_FLAGS)
        }
    }
} catch {}
# (TODO 382) The small window's own log (the game starts it; the setup and the update have theirs): logs\hd_textures.log
# in the install folder (hd_textures\hd_textures.log before the install layout), the run before as hd_textures_prev.log.
# And in every run, the graphics cards with their driver versions, for a report.
if ($Resume -or $Verify) {
    try {
        $logDir = Get-LayoutLogsDir $Root
        if (-not $logDir) { $logDir = $Out; New-Item -ItemType Directory -Force $logDir | Out-Null }
        $makerLog = Join-Path $logDir "hd_textures.log"
        if (Test-Path -LiteralPath $makerLog) { Move-Item -LiteralPath $makerLog -Destination (Join-Path $logDir "hd_textures_prev.log") -Force }
        Start-Transcript -Path $makerLog -Force | Out-Null
    } catch {}
}
try {
    $cards = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" })
    if ($cards) { Write-Host "Graphics: $($cards -join '; ')"; Write-StepLog "hd: graphics: $($cards -join '; ')" }
} catch {}
# (TODO 382 (b)) Each card's own video memory (the driver's record) and how much is in use now (Windows' GPU counters,
# by class name: the same on every Windows language): a card short of memory - the game open beside the upscaler -
# shows here.
try {
    $mem = @()
    foreach ($k in Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction SilentlyContinue) {
        $p = Get-ItemProperty -LiteralPath $k.PSPath -ErrorAction SilentlyContinue
        if (-not $p -or -not $p.DriverDesc) { continue }
        $bytes = 0
        if ($p.'HardwareInformation.qwMemorySize') { $bytes = [uint64]$p.'HardwareInformation.qwMemorySize' }
        elseif ($p.'HardwareInformation.MemorySize' -is [byte[]]) { $bytes = [BitConverter]::ToUInt32($p.'HardwareInformation.MemorySize', 0) }
        elseif ($p.'HardwareInformation.MemorySize') { $bytes = [uint64]$p.'HardwareInformation.MemorySize' }
        if ($bytes -gt 0) { $mem += "$($p.DriverDesc) $([int64]($bytes / 1MB)) MB" }
    }
    $used = @()
    try {
        $used = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory -ErrorAction Stop |
            ForEach-Object { [int64]($_.DedicatedUsage / 1MB) } | Sort-Object -Descending)
    } catch {}
    $memLine = "video memory: $(if ($mem) { $mem -join '; ' } else { 'not read' }); in use now: " +
        $(if ($used) { "$($used[0]) MB (the busiest card)" } else { "not read" })
    Write-Host "  ($memLine)"; Write-StepLog "hd: $memLine"
} catch {}
# (TODO 267) Beside the game (-GamePid): never waiting for it; the upscale stops when it ends.
$withGame = if ($GamePid -gt 0) { @("--game-pid", "$GamePid") } else { @() }
# (TODO 371) Windows 11's Smart App Control refusing the upscaler (realesrgan-ncnn-vulkan.exe, downloaded) or a Python
# library's file (numpy, Pillow, scipy: unsigned): it lets run only what is signed or what Microsoft's cloud already
# knows. Not a failed setup or update: no HD textures, the game draws its own, and F1 makes them later. Exit 7 tells
# setup.ps1 / update.ps1 so. The marker below starts the message the trap knows it by.
$SmartAppMarker = "Windows' Smart App Control blocked"
function Find-SmartAppBlock([string]$Text) {
    if ($Text -notmatch "Application Control policy has blocked|WinError 4551|0xc0e90002") { return $null }
    $what = "a program or library the HD textures need"
    $m = [regex]::Match($Text, "while importing (\S+?):")
    if ($m.Success) { $what = "a Python library's file ($($m.Groups[1].Value)) the HD textures need" }
    elseif ($Text -match "realesrgan") { $what = "the texture upscaler (realesrgan-ncnn-vulkan.exe)" }
    return "$SmartAppMarker $what. No HD textures were made: the game runs with its own textures. Once Smart App Control is " +
        "off (Windows Security > App & browser control > Smart App Control settings; on some Windows 11 versions it cannot " +
        "be turned back on without resetting Windows), F1 > Graphics > Make HD textures makes them. To see every file it " +
        "blocked: Event Viewer > Applications and Services Logs > Microsoft > Windows > CodeIntegrity > Operational, event 3077."
}
# (in that window a stop is shown for a minute before it closes; otherwise an error ends the script as always)
trap {
    if ("$_".StartsWith($SmartAppMarker)) {
        Write-Host "`nwarning: $_" -ForegroundColor Yellow
        if (Get-Command SetStatus -ErrorAction SilentlyContinue) { try { SetStatus "card_broken" } catch {} } # (not tried again at each start)
        if ($Resume) { Write-Host "This window closes in a minute."; Start-Sleep -Seconds 60 }
        exit 7
    }
    if ($Verify) {
        Write-Host "`nVerifying the HD textures stopped: $_" -ForegroundColor Red
        Write-Host "Nothing was changed. This window closes in a minute."
        Start-Sleep -Seconds 60
    }
    elseif ($Resume) {
        Write-Host "`nMaking the HD textures stopped: $_" -ForegroundColor Red
        Write-Host "It is tried again the next time you start the game. This window closes in a minute."
        Start-Sleep -Seconds 60
    }
    break
}

# (TODO 219, PR #20) The choices (the setup writes them): Fast, Balanced or Best, and whether to keep making them while
# the game runs. An options file from before Balanced says "best" or "fast": both are still presets.
$OptionsFile = Join-Path $Out "hd_options.json"
$StatusFile = Join-Path $Out "upscale_status.json"
if (-not $Quality) {
    $Quality = "balanced"
    if (Test-Path $OptionsFile) {
        try { $o = Get-Content $OptionsFile -Raw | ConvertFrom-Json; if ("$($o.quality)" -in @("fast", "balanced", "best")) { $Quality = "$($o.quality)" } } catch {}
    }
}
function SetStatus([string]$State) {
    # (the upscaler keeps the counts in it; this marks the whole set's state: "done" once everything is made)
    $st = [ordered]@{ state = $State; done = 0; total = 0; quality = $Quality }
    if (Test-Path $StatusFile) {
        try {
            $old = Get-Content $StatusFile -Raw | ConvertFrom-Json; $st.done = $old.done; $st.total = $old.total
            if ($old.gpu) { $st.gpu = $old.gpu }
            # (TODO 382: why the upscale stopped on the card, kept while the set stays unfinished)
            if ($State -ne "done" -and $old.stop_reason) { $st.stop_reason = $old.stop_reason; $st.driver = $old.driver }
        } catch {}
    }
    $st.updated = (Get-Date).ToUniversalTime().ToString("s")
    # (UTF-8 without a BOM: the game reads it. Written beside it and put in place in one step: never left half-written)
    $tmp = "$StatusFile.tmp"
    [System.IO.File]::WriteAllText($tmp, ($st | ConvertTo-Json))
    for ($i = 0; $i -lt 20; $i++) { try { Move-Item -LiteralPath $tmp -Destination $StatusFile -Force -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 100 } }
}
# One maker at a time: the setup's, Play.bat's or the game's. hd_textures\.upscaling holds its process id while it runs;
# Windows deletes the file when it ends (a closed window too).
$script:WorkerLock = $null
function TakeWorkerLock {
    $path = Join-Path $Out ".upscaling"
    New-Item -ItemType Directory -Force $Out | Out-Null
    for ($i = 0; $i -lt 2; $i++) {
        try {
            $script:WorkerLock = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
                ([System.IO.FileShare]::Read -bor [System.IO.FileShare]::Delete), 4096, [System.IO.FileOptions]::DeleteOnClose)
            $b = [System.Text.Encoding]::ASCII.GetBytes("$PID")
            $script:WorkerLock.Write($b, 0, $b.Length); $script:WorkerLock.Flush()
            return $true
        } catch { Start-Sleep -Milliseconds 300 }
    }
    return $false
}

# (TODO 352) The small window (-Resume) says in its title where it is, read on the taskbar while it is minimized:
# the step, then "HD textures: N of M made", "waiting for the game to close", and "ready" at the end.
function HdTitle([string]$Text) { if ($Resume -and -not $Verify) { try { $Host.UI.RawUI.WindowTitle = $Text } catch {} } }
function Step($msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan; Write-StepLog "hd: $msg"
    HdTitle ("HD textures: " + ($msg -replace ' into .*$', '' -replace ' \(.*$', ''))
}
function Run([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = "Continue"
    $seen = New-Object System.Collections.Generic.List[string]
    try { & $Exe @Arguments 2>&1 | ForEach-Object { $l = "$_"; $seen.Add($l); $l } }
    catch { $b = Find-SmartAppBlock "$_"; if ($b) { throw $b }; throw }
    if ($LASTEXITCODE -ne 0) {
        $b = Find-SmartAppBlock ($seen -join "`n"); if ($b) { throw $b }  # (TODO 371)
        throw "$Exe failed with exit code $LASTEXITCODE"
    }
}

# (TODO 382) A step that must never hold the setup for good: the program run as a child of this script, its output
# shown as it comes, and after -Seconds that child - only it, the process this function started, by its handle and its
# start time - ended. Returns its exit code, or $null when the limit ended it. -Hang (tests, TF_HD_TEST_HANG): the
# program replaced by a Python that only sleeps, to drive the limit.
function Format-WinArg([string]$a) {
    # (one argument as CreateProcess's command line reads it back: quoted when it holds a space or a quote,
    # backslashes before a quote doubled; Windows PowerShell's .NET has no ArgumentList)
    if ($a -ne "" -and $a -notmatch '[\s"]') { return $a }
    $s = '"'; $bs = 0
    foreach ($c in $a.ToCharArray()) {
        if ($c -eq '\') { $bs++; continue }
        if ($c -eq '"') { $s += ('\' * ($bs * 2 + 1)) + '"' } else { $s += ('\' * $bs) + $c }
        $bs = 0
    }
    $s + ('\' * ($bs * 2)) + '"'
}
function Invoke-Limited([string]$Exe, [string[]]$Arguments, [int]$Seconds, [switch]$Quiet, [switch]$Hang) {
    if ($Hang) { $Arguments = @("-c", "import time; time.sleep(100000)") }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (@($Arguments | ForEach-Object { Format-WinArg "$_" }) -join " ")
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    $seen = New-Object System.Collections.Generic.List[string]
    try {
        try { [void]$p.Start() } catch { $b = Find-SmartAppBlock "$_"; if ($b) { throw $b }; throw }
        $started = $p.StartTime
        # (both outputs read line by line as they come, without PowerShell events - those wait for the script)
        $readers = @($p.StandardOutput, $p.StandardError)
        $tasks = @($readers[0].ReadLineAsync(), $readers[1].ReadLineAsync())
        $open = @($true, $true)
        $until = (Get-Date).AddSeconds($Seconds)
        while ($open[0] -or $open[1] -or -not $p.HasExited) {
            $got = $false
            for ($k = 0; $k -lt 2; $k++) {
                if ($open[$k] -and $tasks[$k].IsCompleted) {
                    $line = $tasks[$k].Result
                    if ($null -eq $line) { $open[$k] = $false } else { $seen.Add($line); if (-not $Quiet) { Write-Host $line }; $tasks[$k] = $readers[$k].ReadLineAsync() }
                    $got = $true
                }
            }
            if ((Get-Date) -ge $until) {
                # (ended already - a child of its own still holding the output open: its code counts)
                if ($p.HasExited) { $script:LimitedLines = $seen; return $p.ExitCode }
                # (still the process started above: its handle is held, so its id cannot have gone to another)
                if (-not $p.HasExited -and $p.StartTime -eq $started) { try { $p.Kill() } catch {} }
                [void]$p.WaitForExit(10000)
                $script:LimitedLines = $seen
                return $null
            }
            if (-not $got) { Start-Sleep -Milliseconds 50 }
        }
        $p.WaitForExit()
        $script:LimitedLines = $seen
        return $p.ExitCode
    } finally {
        $p.Dispose()
    }
}# (the limits, in seconds; TF_HD_LIMIT_<NAME> for tests)
function Limit([string]$Name, [int]$Default) {
    $v = [Environment]::GetEnvironmentVariable("TF_HD_LIMIT_$Name")
    if ($v -match '^\d+$') { return [int]$v } else { return $Default }
}
function Hangs([string]$Name) { return ($env:TF_HD_TEST_HANG -split ',') -contains $Name }
# A limit as words: "15 minutes", "2 minutes", "20 seconds".
function Format-Limit([int]$Seconds) {
    if ($Seconds -ge 60 -and $Seconds % 60 -eq 0) { $m = $Seconds / 60; return "$m minute$(if ($m -ne 1) { 's' })" }
    return "$Seconds second$(if ($Seconds -ne 1) { 's' })"
}

# (TODO 186) numpy, which the sharp fonts need, installed when it is missing; $true when it is there.
# (TODO 382) Neither the import check (2 min) nor pip (15 min) may hold the setup for good. A limit that ended it is
# said here, in full ($script:NumpyWarned): the caller then adds no second warning.
$script:NumpyWarned = $false
function EnsureNumpy {
    $limit = Limit "NUMPY" 120
    $code = Invoke-Limited $Python @("-c", "import numpy") $limit -Quiet -Hang:(Hangs "numpy")
    if ($null -eq $code) {
        Write-Host "`nwarning: Python took over $(Format-Limit $limit) just to load numpy (an antivirus checking it, most likely); the sharp text and PS5 button symbols are left for the next run." -ForegroundColor Yellow
        Write-StepLog "hd: numpy's import stopped after its time limit"
        $script:NumpyWarned = $true
        return $false
    }
    if ($code -eq 0) { return $true }
    Step "Installing numpy (Python), for the sharp fonts"
    $limit = Limit "PIP" 900
    $code = Invoke-Limited $Python @("-m", "pip", "install", "--user", "numpy") $limit -Hang:(Hangs "pip")
    if ($null -eq $code) {
        Write-Host "`nwarning: installing numpy took over $(Format-Limit $limit) and was stopped; the sharp text and PS5 button symbols are left for the next run." -ForegroundColor Yellow
        $script:NumpyWarned = $true
        Write-StepLog "hd: pip install numpy stopped after its time limit"
        return $false
    }
    $code = Invoke-Limited $Python @("-c", "import numpy") (Limit "NUMPY" 120) -Quiet
    return ($code -eq 0)
}
# (TODO 382) Python's, numpy's and Pillow's versions in the window and the step log, once a run: what a report needs.
$script:VersionsWritten = $false
function Write-PythonVersions {
    if ($script:VersionsWritten) { return }
    $script:VersionsWritten = $true
    $code = Invoke-Limited $Python @("-c", "import sys, numpy, PIL; print('Python %s, numpy %s, Pillow %s' % (sys.version.split()[0], numpy.__version__, PIL.__version__))") (Limit "NUMPY" 120) -Quiet
    $v = if ($code -eq 0 -and $script:LimitedLines.Count) { $script:LimitedLines[$script:LimitedLines.Count - 1] } else { "Python's versions not read" }
    Write-Host "  ($v)"
    Write-StepLog "hd: $v"
}

# Only FONTS from TFA2.ZIP: no other archives, downloads or GPU upscale.
if ($FontsOnly) {
    try {
        if (-not (EnsureNumpy)) {
            if ($script:NumpyWarned) { return }  # (its time limit, already said)
            throw "Python's numpy could not be installed or loaded (pip install numpy)"
        }
        Write-PythonVersions
        # (TODO 382: never longer than 15 minutes; it takes about a minute)
        $limit = Limit "FONTS" 900
        $code = Invoke-Limited $Python @((Join-Path $Py "make_fonts_only.py"), "--game", $Game, "--out", $Out, "--controls", (Join-Path $Root "controls.txt")) $limit -Hang:(Hangs "fonts")
        if ($null -eq $code) { throw "making them took over $(Format-Limit $limit) and was stopped (it is tried again next time)" }
        if ($code -ne 0) { $b = Find-SmartAppBlock ($script:LimitedLines -join "`n"); if ($b) { throw $b }; throw "make_fonts_only.py failed with exit code $code" }
    } catch {
        Write-Host "`nwarning: $($_.Exception.Message): the button prompts and sharp fonts are not made; continuing with the game's own fonts." -ForegroundColor Yellow
    }
    return
}

# (User decision 2026-11-11) Built-in or low-end graphics (no card with 3.5 GB of its own video memory: the GTX 1650's
# 4 GB and up are not): no HD textures are made here - the upscaler and the streaming of large images make the game
# stutter or crash on them. Once, a set made before is removed (the game then draws its own textures) and the space
# it took is said; hd_textures\.low_end_cleared marks that it was done, so it is never done twice. The sharp fonts
# (no graphics card work) are still made. The card: the game writes hd_textures\.gpu.json at each start (the card it
# draws on); before the game has run, the cards Windows lists. TF_HD_LOW_END=0 / 1 decides for tests.
function Get-LowEndGraphics {
    if ($env:TF_HD_LOW_END -in "0", "1") { return @{ low = ($env:TF_HD_LOW_END -eq "1"); card = "(TF_HD_LOW_END)"; mb = 0 } }
    $gpuFile = Join-Path $Out ".gpu.json"
    if (Test-Path $gpuFile) {
        try { $g = Get-Content $gpuFile -Raw | ConvertFrom-Json; return @{ low = [bool]$g.low_end; card = [string]$g.card; mb = [int64]$g.video_memory_mb } } catch {}
    }
    $best = $null
    foreach ($k in Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction SilentlyContinue) {
        $p = Get-ItemProperty -LiteralPath $k.PSPath -ErrorAction SilentlyContinue
        if (-not $p -or -not $p.DriverDesc -or $p.DriverDesc -match 'Basic|Parsec|Virtual|Remote|Citrix|Mirage|Meta') { continue }
        $bytes = 0
        if ($p.'HardwareInformation.qwMemorySize') { $bytes = [uint64]$p.'HardwareInformation.qwMemorySize' }
        elseif ($p.'HardwareInformation.MemorySize' -is [byte[]]) { $bytes = [BitConverter]::ToUInt32($p.'HardwareInformation.MemorySize', 0) }
        elseif ($p.'HardwareInformation.MemorySize') { $bytes = [uint64]$p.'HardwareInformation.MemorySize' }
        $mb = [int64]($bytes / 1MB)
        if (-not $best -or $mb -gt $best.mb) { $best = @{ card = [string]$p.DriverDesc; mb = $mb } }
    }
    if (-not $best) { return @{ low = $false; card = ""; mb = 0 } }  # (nothing found: nothing changes)
    return @{ low = ($best.mb -lt 3584); card = $best.card; mb = $best.mb }
}
if (-not $FontsOnly -and -not $CrispFontsOnly) {
    $lowEnd = Get-LowEndGraphics
    if ($lowEnd.low) {
        $mark = Join-Path $Out ".low_end_cleared"
        if (-not (Test-Path $mark)) {
            $bytes = 0
            foreach ($d in "upscaled", "upscaled_precolour", "upscaled_bc7_cache", "upscaled_orig", "work") {
                $dir = Join-Path $Out $d
                if (Test-Path -LiteralPath $dir) {
                    $bytes += [int64](Get-ChildItem -LiteralPath $dir -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
                    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
            foreach ($f in "upscale_status.json", ".images_checked_3") { Remove-Item -LiteralPath (Join-Path $Out $f) -Force -ErrorAction SilentlyContinue }
            # (the HD textures setting off: the game's own textures, and F1 shows it so)
            $ini = Join-Path $Root "settings.ini"
            $lines = @(if (Test-Path $ini) { Get-Content -LiteralPath $ini } else { @() })
            $lines = @($lines | Where-Object { $_ -notmatch '^\s*TF_HIRES\s*=' }) + @("TF_HIRES=0")
            Set-Content -LiteralPath $ini -Value $lines
            New-Item -ItemType Directory -Force $Out | Out-Null
            Set-Content -LiteralPath $mark -Value "cleared $(Get-Date -Format s) on $($lowEnd.card) ($($lowEnd.mb) MB): $bytes bytes"
            $size = if ($bytes -ge 1GB) { "{0:N1} GB" -f ($bytes / 1GB) } elseif ($bytes -gt 0) { "{0:N0} MB" -f ($bytes / 1MB) } else { "" }
            if ($size) { Write-Host "==> HD textures off on built-in graphics: $size of HD textures removed (the game's own textures are used)" }
            else { Write-Host "==> HD textures off on built-in graphics (the game's own textures are used)" }
        }
        if ($Resume -or $Verify) { Write-Host "Built-in or low-end graphics ($($lowEnd.card)): no HD textures are made on this PC."; return }
        Write-Host "Built-in or low-end graphics ($($lowEnd.card), $($lowEnd.mb) MB of its own video memory): no HD textures are made on this PC; the sharp fonts are."
        $SkipUpscale = $true
    }
}

if (-not $CrispFontsOnly -and -not (TakeWorkerLock)) {
    if ($Resume -or $Verify) { Write-Host "The HD textures are being made already (another window)."; return }
    # (the setup or an update while the game's own window makes them: it waits for that one, then carries on)
    Write-Host "The HD textures are being made in another window: waiting for it (close it to carry on here)."
    while (-not (TakeWorkerLock)) { Start-Sleep -Seconds 5 }
}
# (TODO 281) hd_textures\work holds only the batch being made, and under the lock none is: what is there was left by a
# stop or by an older maker (a player's set had 2.4 GB of one batch, named before the batches were numbered, that
# nothing ever deleted). Removed at every run - the setup, an update, the game's window, Verify.
$workDir = Join-Path $Out "work"
if (-not $CrispFontsOnly -and (Test-Path -LiteralPath $workDir)) {
    $stale = @(Get-ChildItem -LiteralPath $workDir -Force -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) {
        $bytes = ($stale | ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force -ErrorAction SilentlyContinue } |
            Measure-Object Length -Sum).Sum
        $stale | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
        Write-Host ("Removed {0} of leftover work files (hd_textures\work)." -f $(if ($bytes -ge 1GB) { "{0:N1} GB" -f ($bytes / 1GB) } else { "{0:N0} MB" -f ($bytes / 1MB) }))
    }
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

if ($Verify) {
    Step "Verifying the HD textures (nothing is changed)"
    $a = @((Join-Path $Py "upscale_textures.py"), $converted, "--out", (Join-Path $Out "upscaled"), "--verify",
        "--quality", $Quality, "--hd-dir", $Out)
    Run $Python $a
    Write-Host "`nDone: see F1 > Graphics." -ForegroundColor Green
    return
}

function CrispFonts {
    # 5. (TODO 119) The font atlases made again sharp at 8x - the menu font from TeX Gyre Adventor (GUST Font
    #    License, tools\hd_textures\typeface), the HUD's and HQ's rebuilt with sharp edges - with the button
    #    symbols drawn in the DualSense's style, into hd_textures\crisp_fonts (make_crisp_fonts.py). The
    #    game's Display settings "Sharp text" / "Button symbols" choose among them. Needs numpy; a minute.
    # (TODO 186: numpy installed here when it is missing - setup.ps1 never installed it, so an install made
    # without it never had the sharp text: the game showed the old letters)
    if (-not (EnsureNumpy)) {
        if (-not $script:NumpyWarned) {  # (a time limit is already said)
            Write-Host "`nwarning: Python's numpy could not be installed or loaded (pip install numpy): the sharp text and PS5 button symbols are not made." -ForegroundColor Yellow
        }
        return
    }
    Write-PythonVersions
    Step "Making the sharp fonts and PS5 button symbols into $(Join-Path $Out 'crisp_fonts')"
    $a = @((Join-Path $Py "make_crisp_fonts.py"), "--archives", $archives, "--converted", $converted, "--out", (Join-Path $Out "crisp_fonts"))
    $up = Join-Path $Out "upscaled"
    if (Test-Path $up) { $a += @("--upscaled", $up) }
    $controls = Join-Path $Root "controls.txt"  # (the keyboard layout: the key prompts' keys)
    if (Test-Path $controls) { $a += @("--controls", $controls) }
    # (TODO 382, public issue #32: a player's maker stopped at this step on every run - it comes first while no
    # manifest is there. It takes about a minute; after 15 it is ended, said plainly, and the HD textures go on. No
    # manifest is written then, so the next run tries again.)
    $limit = Limit "FONTS" 900
    $code = Invoke-Limited $Python $a $limit -Hang:(Hangs "fonts")
    if ($null -eq $code) {
        Write-Host ("`nwarning: making the sharp text and PS5 button symbols took over $(Format-Limit $limit) and was stopped. The game " +
            "keeps its own text and button symbols for now; it is tried again the next time the HD textures are made. " +
            "If this happens every time, please tell us (the logs folder's update.log or setup.log).") -ForegroundColor Yellow
        Write-StepLog "hd: the sharp fonts stopped after their time limit"
        return
    }
    if ($code -ne 0) {
        $b = Find-SmartAppBlock ($script:LimitedLines -join "`n"); if ($b) { throw $b }  # (TODO 371)
        # (TODO 382: a failed fonts step is not the end of the HD textures: said plainly, its last lines kept for a report)
        $tail = @($script:LimitedLines | Select-Object -Last 6)
        Write-Host ("`nwarning: making the sharp text and PS5 button symbols failed (exit code $code). The game keeps its own " +
            "text and button symbols for now; it is tried again the next time the HD textures are made.") -ForegroundColor Yellow
        if ($tail) { Write-Host "  its last lines:"; $tail | ForEach-Object { Write-Host "    $_" } }
        Write-StepLog "hd: the sharp fonts failed (exit code $code): $($tail -join ' | ')"
        return
    }
    Write-StepLog "hd: the sharp fonts made"
}

if ($CrispFontsOnly) { CrispFonts; return }
if ($SkipUpscale) { CrispFonts; return }

# (TODO 219) The sharp fonts first, when there are none yet: a minute, and the game started while the rest is made
# (the setup's "Play now", Play.bat) has sharp menus from the start. Made again at the end, with the upscaled set.
if (-not (Test-Path (Join-Path $Out "crisp_fonts\manifest.json"))) { CrispFonts }

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

# (TODO 241) A set made before the maker checked its images: every image checked once (a graphics card that gives
# black, junk-striped or red-and-blue-exchanged images - a GTX 1660 SUPER did): exchanged ones put right, broken ones
# removed and made again below. Again whenever "Make HD textures" (F1) runs this on a set the card made broken or set aside.
$checkedMark = Join-Path $Out ".images_checked_3"
$finished = $false
# (PR #20: a finished set made for another preset is shrunk or made larger below, and those images are checked then)
if (Test-Path $StatusFile) {
    try {
        $st0 = Get-Content $StatusFile -Raw | ConvertFrom-Json
        $finished = ($st0.state -in @("done", "card_broken")) -and (-not $st0.quality -or "$($st0.quality)" -eq $Quality)
    } catch {}
}
if ($Remake) {
    Step "Removing the images the last verify found missing or broken (they are made again below)"
    Run $Python @((Join-Path $Py "upscale_textures.py"), $converted, "--out", (Join-Path $Out "upscaled"), "--forget-broken",
        "--hd-dir", $Out)
}
elseif ((Test-Path (Join-Path $Out "upscaled\upscale_manifest.jsonl")) -and (-not (Test-Path $checkedMark) -or ($Resume -and $finished))) {
    Step "Checking the HD textures made so far (a few minutes)"
    Run $Python (@((Join-Path $Py "upscale_textures.py"), $converted, "--out", (Join-Path $Out "upscaled"), "--check-colours") +
        $(if ($GamePid -gt 0) { @("--no-wait-for-game") } else { @() }))
    Set-Content -LiteralPath $checkedMark -Value "checked $(Get-Date -Format s)"
}

Step "Upscaling into $(Join-Path $Out 'upscaled') (resumable)"
# A few textures failing is not a failed setup: the game keeps the original for each texture without
# an upscale, and a run again tries only those. (Exit 1 with its "upscaled N/M" summary = some files
# failed; anything else - no summary, exit 2 - is a real failure.)
$ErrorActionPreference = "Continue"
# (TODO 219) In batches, the textures seen first made first, each colour-matched as it is made; it waits while the
# game runs (unless hd_options.json says to carry on) and says "HD textures: N of M made" after each batch.
& $Python (Join-Path $Py "upscale_textures.py") $converted --out (Join-Path $Out "upscaled") `
    --work (Join-Path $Out "work") --exe $exe --quality $Quality --match-colour @withGame @(if ($Gentle) { "--gentle" }) @(if ($RetryResets) { "--retry-resets" }) 2>&1 | ForEach-Object {
        $l = "$_"
        if ($l.StartsWith("HD textures: ")) { Write-StepLog "hd:     $l"; HdTitle $l }   # (TODO 310: the batch it was at)
        elseif ($l -match '^paused while the game runs \((.*)\)') { HdTitle "HD textures: waiting for the game to close ($($Matches[1]))" }
        elseif ($l -eq "the game closed: carrying on") { HdTitle "HD textures: being made now" }
        # (TODO 382) The card the upscaler picked, an image marked because it stops the upscaler, and the run's count
        # of them: in the step log too
        elseif ($l -match "^note: the upscaler's graphics card|^note: no HD image for|^warning: \d+ textures? could not be made|^note: the upscale stopped") { Write-StepLog "hd: $l" }
        $l
    } | Tee-Object -Variable lines | Out-Host
$code = $LASTEXITCODE
# (TODO 382) The upscaler's exit code and, when it did not end well, its last lines - in the step log too, for a report
if ($code -ne 0 -and $code -ne 6) {  # (6: nothing was tried; its note above says why)
    $tail = @($lines | ForEach-Object { "$_" } | Where-Object { $_ -notmatch 'DeprecationWarning|getdata' } | Select-Object -Last 8)
    Write-Host "  (the upscaler ended with exit code $code; its last lines:)"; $tail | ForEach-Object { Write-Host "    $_" }
    Write-StepLog "hd: the upscaler ended with exit code ${code}: $($tail -join ' | ')"
}
# (TODO 382) Exit 6: this card's driver was reset in each of the last runs: nothing tried (the upscaler set the state
# "resets_stopped", the game shows it); said once already, so quietly - no colour match, no fonts, no window held.
if ($code -eq 6) {
    # (said as it was: a reset Windows logged, or the upscaler losing its card with no reset logged)
    $lost = $false
    try { $lost = ("$((Get-Content $StatusFile -Raw | ConvertFrom-Json).stop_reason)" -eq "device_lost") } catch {}
    if ($lost) {
        Write-StepLog "hd: not tried again: the upscaler lost its graphics card in the last runs, no driver reset logged (F1 > Make HD textures tries again)"
        HdTitle "HD textures: stopped - the upscaler stops on this graphics driver"
    }
    else {
        Write-StepLog "hd: not tried again: graphics driver resets in the last runs (F1 > Make HD textures tries again)"
        HdTitle "HD textures: stopped - the graphics driver was reset in every run"
    }
    exit 0
}
# (TODO 267) Exit 4: the game closed and the upscale stopped with it - the rest waits for the next start.
if ($code -eq 4) { exit 0 }
# (0.7.0) Exit 5: the graphics driver was reset even with the smallest pieces, so this run stopped with images left.
# The set stays unfinished (never "done"): the next run - the game's next start, an update, F1 - carries on.
$stoppedByResets = ($code -eq 5)
if ($stoppedByResets) {
    # (TODO 382: as it was - a driver reset Windows logged, or only the upscaler losing its card on this driver)
    $why = "the graphics driver was reset while the HD textures were made"
    try { if ((Get-Content $StatusFile -Raw | ConvertFrom-Json).stop_reason -eq "device_lost") { $why = "the upscaler lost its graphics card on this graphics driver (no driver reset was logged)" } } catch {}
    Write-Host "`nwarning: $why, even in the smallest pieces:" -ForegroundColor Yellow
    Write-Host "this run stopped with some left to make. What is made is kept and used; the rest is made the next time." -ForegroundColor Yellow
    $code = 0
}
$ErrorActionPreference = "Stop"
$sacBlock = if ($code -ne 0) { Find-SmartAppBlock (($lines | ForEach-Object { "$_" }) -join "`n") } else { $null }
if ($sacBlock) { throw $sacBlock }  # (TODO 371: the trap says it and ends with 7)
$summary = $lines | Where-Object { $_ -match '^upscaled \d+/\d+ uniques' } | Select-Object -Last 1
# (TODO 242) Exit 3: this graphics card's upscales come out broken in every way the upscaler has (a GTX 1660 SUPER's
# were all black). Nothing is made on it - the game draws its own textures - and the next run tries the card again.
$cardBroken = ($code -eq 3)
if ($cardBroken) {
    Write-Host "`nwarning: this graphics card's HD images come out broken (black), in every way the upscaler can make them." -ForegroundColor Yellow
    Write-Host "No HD textures are made on it; the game draws its own. A newer graphics driver may fix it:" -ForegroundColor Yellow
    Write-Host "then F1 > Graphics > Make HD textures tries again." -ForegroundColor Yellow
}
elseif ($code -ne 0) {
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
# (--only-new: the images matched as they were made are not measured again)
Run $Python @((Join-Path $Py "match_colour.py"), "--upscaled", (Join-Path $Out "upscaled"), "--native", $converted, "--only-new")
CrispFonts
# (TODO 382) "resets_stopped": the upscaler gave up on this card after runs in a row stopped by resets - kept as it says
$resetsGaveUp = $false; $stopReason = ""
if ($stoppedByResets) {
    try { $sj = Get-Content $StatusFile -Raw | ConvertFrom-Json; $resetsGaveUp = ($sj.state -eq "resets_stopped"); $stopReason = "$($sj.stop_reason)" } catch {}
}
# (TODO 382) What it was, in the words the player can check: a reset Windows logged (the screen goes black), or only the
# upscaler losing its card on this graphics driver (nothing to see: another driver version is the thing to try).
$stopWhat = if ($stopReason -eq "device_lost") { "the upscaler lost its graphics card on this graphics driver (no driver reset was logged; another driver version may help)" } else { "the graphics driver was reset during the upscale" }
SetStatus $(if ($cardBroken) { "card_broken" } elseif ($resetsGaveUp) { "resets_stopped" } elseif ($stoppedByResets) { "working" } else { "done" })
Write-Host "`nDone. Start the game from this folder (Play.bat): the log says '[native render] upscaled textures: N images'." -ForegroundColor Green
# (TODO 352) The small window: a done message the player sees - shown again (not taking the focus) and left up a while.
# (Not after a run the graphics driver's resets stopped with images left: that set is not ready.)
if ($stoppedByResets) { HdTitle "HD textures: not finished - the next start of the game carries on" }
Write-StepLog ("hd: the run ended: " + $(if ($cardBroken) { "nothing made on this card (broken images)" } elseif ($stoppedByResets) { "not finished (graphics driver resets)" } else { "ready" }))
# (TODO 382) The small window, after a run the graphics driver's resets stopped: said, and held open like the done
# message (it closed at once before, and looked like a crash at the last line shown - the sharp fonts).
$heldReason = if ($Resume -and -not $Verify -and $stoppedByResets) { "not finished" } else { "" }
if ($heldReason -and $resetsGaveUp) {
    HdTitle "HD textures: stopped - the graphics driver was reset in every run"
    Write-Host ("`nThe HD textures are not made further on this PC: in each of the last runs $stopWhat, even with the " +
        "smallest pieces. What is made is used, with the game's own textures for the rest, and no run tries again by " +
        "itself. F1 > Graphics > Make HD textures tries again (after a graphics driver change, the next start does). If " +
        "you tell us, please attach logs\hd_textures.log from the install folder.") -ForegroundColor Yellow
}
elseif ($heldReason) {
    Write-Host ("`nThe HD textures are not finished: $stopWhat. What is made is used; " +
        "the rest is made the next time you start the game. If this happens every time, please tell us and attach " +
        "logs\hd_textures.log from the install folder.") -ForegroundColor Yellow
}
if ($Resume -and -not $Verify -and -not $cardBroken -and ($heldReason -or -not $stoppedByResets)) {
    if (-not $heldReason) {
        HdTitle "HD textures: ready - start the game to use them"
        Write-Host "`nThe HD textures are ready. Start the game again to use them." -ForegroundColor Green
    }
    try {
        if (-not ("TfHd.TfHdWindow" -as [type])) {
            Add-Type -Namespace TfHd -Name TfHdWindow -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern System.IntPtr GetConsoleWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr hwnd, int command);
'@
        }
        [TfHd.TfHdWindow]::ShowWindow([TfHd.TfHdWindow]::GetConsoleWindow(), 4) | Out-Null   # (SW_SHOWNOACTIVATE)
    } catch {}
    Write-Host "This window closes in 5 minutes, or press Enter."
    $until = (Get-Date).AddMinutes(5)
    try {
        while ((Get-Date) -lt $until) {
            if ([Console]::KeyAvailable -and [Console]::ReadKey($true).Key -eq [ConsoleKey]::Enter) { break }
            Start-Sleep -Milliseconds 250
        }
    } catch { Start-Sleep -Seconds 300 }
}
