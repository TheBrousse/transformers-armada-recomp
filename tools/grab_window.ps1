<#
.SYNOPSIS
    Saves screenshots of an already running TransformersRecomp window (debugging aid).

.EXAMPLE
    .\tools\grab_window.ps1 -Count 10 -IntervalMs 100 -Name burst   # runs\burst_00.png ...
#>
param(
    [int]$Count = 1,
    [int]$IntervalMs = 100,
    [string]$Name = "grab"
)

$Root = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $Root "runs"
New-Item -ItemType Directory -Force $runs | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class TfGrab {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@

$p = Get-Process TransformersRecomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p -or $p.MainWindowHandle -eq [IntPtr]::Zero) { Write-Host "no TransformersRecomp window"; exit 1 }
$h = $p.MainWindowHandle
for ($i = 0; $i -lt $Count; $i++) {
    $r = New-Object TfGrab+RECT
    [void][TfGrab]::GetClientRect($h, [ref]$r)
    $w = [Math]::Max(1, $r.Right - $r.Left); $hgt = [Math]::Max(1, $r.Bottom - $r.Top)
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][TfGrab]::PrintWindow($h, $hdc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
    $file = Join-Path $runs ("{0}_{1:D2}.png" -f $Name, $i)
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    if ($i + 1 -lt $Count) { Start-Sleep -Milliseconds $IntervalMs }
}
Write-Host "saved $Count frame(s) as runs\${Name}_NN.png"
