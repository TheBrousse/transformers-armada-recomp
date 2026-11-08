<#
.SYNOPSIS
    Runs TransformersRecomp and saves screenshots of its window (debugging aid).

.EXAMPLE
    .\tools\capture_window.ps1 -At 20,40 -Name shot     # runs\shot_20s.png, runs\shot_40s.png
.EXAMPLE
    .\tools\capture_window.ps1 -At 60,70 -Keys "58:ENTER","65:X"   # press Start at 58 s, Cross at 65 s
#>
param(
    [double[]]$At = @(30),
    [string]$Name = "shot",
    [string[]]$Keys = @(),        # "seconds:KEY", KEY = ENTER, X, SPACE, UP, DOWN, LEFT, RIGHT, ESC, F1
    [string]$Config = "RelWithDebInfo",
    [int]$Monitor = 1,            # window placement (TF_MONITOR); -1 = leave it to the OS
    [switch]$Widescreen           # the game's 16:9 mode (its default; captures are 4:3 unless asked)
)

$Root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $Root "out\build\transformers\$Config\TransformersRecomp.exe"
$runs = Join-Path $Root "runs"
New-Item -ItemType Directory -Force $runs | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class TfWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    // Key down/up through the window's message queue (no focus needed). GLFW
    // maps keys by the scan code in lParam, so include it.
    public static void Press(IntPtr h, uint vk) {
        uint sc = MapVirtualKey(vk, 0);
        PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)(1 | (sc << 16)));
        System.Threading.Thread.Sleep(150);
        PostMessage(h, 0x0101, (IntPtr)vk, (IntPtr)unchecked((int)(1 | (sc << 16) | (1u << 30) | (1u << 31))));
    }
}
"@

# Open the window on another monitor, unfocused, so runs stay out of the way.
if ($Monitor -ge 0) { $env:TF_MONITOR = "$Monitor" }
$callerWidescreen = $env:PS2X_WIDESCREEN
if ($Widescreen) { $env:PS2X_WIDESCREEN = "1" } elseif (-not $env:PS2X_WIDESCREEN) { $env:PS2X_WIDESCREEN = "0" }
# (not the settings panel's choices: settings.ini is the player's)
$callerSettingsFile = $env:PS2X_SETTINGS_FILE
$env:PS2X_SETTINGS_FILE = "0"
$p = Start-Process -FilePath $exe -WorkingDirectory $Root -PassThru `
    -RedirectStandardOutput (Join-Path $runs "$Name.out") -RedirectStandardError (Join-Path $runs "$Name.err")
if ($callerWidescreen) { $env:PS2X_WIDESCREEN = $callerWidescreen } else { Remove-Item Env:PS2X_WIDESCREEN -ErrorAction SilentlyContinue }
if ($callerSettingsFile) { $env:PS2X_SETTINGS_FILE = $callerSettingsFile } else { Remove-Item Env:PS2X_SETTINGS_FILE -ErrorAction SilentlyContinue }
$vk = @{ ENTER = 0x0D; X = 0x58; SPACE = 0x20; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; ESC = 0x1B; F1 = 0x70 }
$events = @()
foreach ($t in $At) { $events += [pscustomobject]@{ T = [double]$t; Kind = "shot"; Key = "" } }
foreach ($k in $Keys) {
    $parts = $k.Split(":")
    $events += [pscustomobject]@{ T = [double]$parts[0]; Kind = "key"; Key = $parts[1].ToUpper() }
}
$start = Get-Date
foreach ($e in ($events | Sort-Object T)) {
    $t = $e.T
    while (((Get-Date) - $start).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 100 }
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Host "no window at ${t}s"; continue }
    if ($e.Kind -eq "key") {
        [TfWin]::Press($h, [uint32]$vk[$e.Key])
        Write-Host "pressed $($e.Key) at ${t}s"
        continue
    }
    $r = New-Object TfWin+RECT
    [void][TfWin]::GetClientRect($h, [ref]$r)
    $w = [Math]::Max(1, $r.Right - $r.Left); $hgt = [Math]::Max(1, $r.Bottom - $r.Top)
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][TfWin]::PrintWindow($h, $hdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
    $file = Join-Path $runs "${Name}_${t}s.png"
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "saved $file"
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
