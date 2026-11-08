# The game's console window on the second monitor, beside the game's window, without taking the
# focus (dot-sourced by run.ps1). The console is started hidden (Start-Process -WindowStyle
# Hidden with redirected output gives a hidden classic console instead of a Windows Terminal
# window, which opens on the main screen and takes the focus; a minimized one takes it too);
# Show-GameConsole finds it, places it and shows it without activating it.

if (-not ("TfConsoleWindow" -as [type])) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class TfConsoleWindow
{
    delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr context);

    // Real pixels on every monitor (the main one is scaled): positions and sizes as they are.
    public static void PerMonitorDpi() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    const uint SWP_NOZORDER = 0x0004, SWP_NOACTIVATE = 0x0010;
    const int SW_SHOWNOACTIVATE = 4;

    static string ClassOf(IntPtr hwnd) { var s = new StringBuilder(256); GetClassName(hwnd, s, 256); return s.ToString(); }
    static string TitleOf(IntPtr hwnd) { var s = new StringBuilder(1024); GetWindowText(hwnd, s, 1024); return s.ToString(); }

    // Console windows (classic or Windows Terminal) with this title, visible or not.
    public static List<IntPtr> Consoles(string title)
    {
        var found = new List<IntPtr>();
        EnumWindows((hwnd, l) =>
        {
            string cls = ClassOf(hwnd);
            if ((cls == "ConsoleWindowClass" || cls == "CASCADIA_HOSTING_WINDOW_CLASS") &&
                string.Equals(TitleOf(hwnd), title, StringComparison.OrdinalIgnoreCase))
                found.Add(hwnd);
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // The process's visible window of this class (the game's GLFW window), or zero.
    public static IntPtr WindowOf(int pid, string cls)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows((hwnd, l) =>
        {
            uint owner;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner == (uint)pid && IsWindowVisible(hwnd) && ClassOf(hwnd) == cls) { result = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static RECT RectOf(IntPtr hwnd) { RECT r; GetWindowRect(hwnd, out r); return r; }

    // Places the window and shows it without activating it.
    public static void Show(IntPtr hwnd, int x, int y, int width, int height)
    {
        Move(hwnd, x, y, width, height);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    public static void Show(IntPtr hwnd) { ShowWindow(hwnd, SW_SHOWNOACTIVATE); }
    public static void Move(IntPtr hwnd, int x, int y, int width, int height)
    {
        SetWindowPos(hwnd, IntPtr.Zero, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
"@
    [TfConsoleWindow]::PerMonitorDpi()
}

# Console windows titled with the executable's path, before the game starts (to tell its own).
function Get-ConsoleWindows([string]$exe) { @([TfConsoleWindow]::Consoles($exe)) }

# Waits (up to $timeoutSeconds) for the game's console and window, then shows the console on the
# second monitor: beside the game's window when it is there too, else in the monitor's corner.
# With one monitor the console is shown where it opened.
function Show-GameConsole([System.Diagnostics.Process]$process, [string]$exe, $before, [int]$timeoutSeconds = 15) {
    $console = [IntPtr]::Zero
    $game = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while ((Get-Date) -lt $deadline -and -not $process.HasExited) {
        if ($console -eq [IntPtr]::Zero) {
            $console = @([TfConsoleWindow]::Consoles($exe) | Where-Object { $before -notcontains $_ }) | Select-Object -First 1
            if (-not $console) { $console = [IntPtr]::Zero }
        }
        if ($game -eq [IntPtr]::Zero) { $game = [TfConsoleWindow]::WindowOf($process.Id, "GLFW30") }
        if ($console -ne [IntPtr]::Zero -and $game -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($console -eq [IntPtr]::Zero) { return }
    $screen = [System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1
    if (-not $screen) { [TfConsoleWindow]::Show($console); return }
    if ($game -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 300 } # (the game moves its window as it opens)
    $area = $screen.WorkingArea
    $g = if ($game -ne [IntPtr]::Zero) { [TfConsoleWindow]::RectOf($game) } else { $null }
    $x, $y, $w, $h = Find-ConsoleSpot $console $area $g
    [TfConsoleWindow]::Show($console, $x, $y, $w, $h)
    # On a monitor with another DPI the console is scaled as it arrives: placed again at its new size.
    $x, $y, $w, $h = Find-ConsoleSpot $console $area $g
    [TfConsoleWindow]::Move($console, $x, $y, $w, $h)
}

# Where the console goes on the monitor's work area without covering the game's window ($g, or
# $null), as x, y, width, height: beside it or in a corner at its own size; else in the largest
# free strip around the game's window, made smaller to fit.
function Find-ConsoleSpot([IntPtr]$console, $area, $g) {
    $c = [TfConsoleWindow]::RectOf($console)
    $w = $c.Right - $c.Left
    $h = $c.Bottom - $c.Top
    $gap = 16
    $spots = @()
    if ($g) {
        $spots += , @(($g.Left - $gap - $w), $g.Top)     # left of the game
        $spots += , @(($g.Right + $gap), $g.Top)         # right of it
        $spots += , @($g.Left, ($g.Top - $gap - $h))     # above it
        $spots += , @($g.Left, ($g.Bottom + $gap))       # below it
    }
    $spots += , @(($area.Left + $gap), ($area.Top + $gap))
    $spots += , @(($area.Right - $gap - $w), ($area.Top + $gap))
    $spots += , @(($area.Left + $gap), ($area.Bottom - $gap - $h))
    $spots += , @(($area.Right - $gap - $w), ($area.Bottom - $gap - $h))
    foreach ($s in $spots) {
        $x, $y = $s
        $inside = $x -ge $area.Left -and $y -ge $area.Top -and ($x + $w) -le $area.Right -and ($y + $h) -le $area.Bottom
        $clear = (-not $g) -or ($x + $w -le $g.Left) -or ($x -ge $g.Right) -or ($y + $h -le $g.Top) -or ($y -ge $g.Bottom)
        if ($inside -and $clear) { return @($x, $y, $w, $h) }
    }
    if (-not $g) { return @(($area.Left + $gap), ($area.Top + $gap), $w, $h) }
    # The strips of the work area left of, right of, above and below the game's window.
    $strips = @(
        @($area.Left, $area.Top, ($g.Left - $area.Left), $area.Height),
        @($g.Right, $area.Top, ($area.Right - $g.Right), $area.Height),
        @($area.Left, $area.Top, $area.Width, ($g.Top - $area.Top)),
        @($area.Left, $g.Bottom, $area.Width, ($area.Bottom - $g.Bottom)))
    $best = $null
    $bestArea = 0
    foreach ($strip in $strips) {
        $sx, $sy, $sw, $sh = $strip
        $fw = [Math]::Min($w, $sw - 2 * $gap)
        $fh = [Math]::Min($h, $sh - 2 * $gap)
        if ($fw -lt 320 -or $fh -lt 160 -or $fw * $fh -le $bestArea) { continue }
        $bestArea = $fw * $fh
        # Next to the game's window, at its top where there is room.
        $fx = if ($sx + $sw -eq $g.Left) { $g.Left - $gap - $fw } else { $sx + $gap }
        $fy = [Math]::Max($sy + $gap, [Math]::Min($g.Top, $sy + $sh - $gap - $fh))
        $best = @($fx, $fy, $fw, $fh)
    }
    if ($best) { return $best }
    return @(($area.Left + $gap), ($area.Top + $gap), $w, $h)
}
