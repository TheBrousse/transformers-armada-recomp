# prune_runs.ps1 - keeps the test-run output folder (runs\) from growing without end.
# Deletes files in runs\ (and its subfolders) older than -Hours (default 48), then removes the folders
# left empty. Your own play sessions are never touched: runs\playtest*.err/.out and runs\playtest_logs\
# are kept whatever their age (the playtest launcher rotates those itself, newest 30).
# Usage: powershell -NoProfile -File tools\prune_runs.ps1 [-Hours 48] [-WhatIf]
# Called by the run launchers before each test run; safe to run by hand at any time.
param([int]$Hours = 48, [switch]$WhatIf)
$ErrorActionPreference = 'Continue'
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) 'runs'
if (-not (Test-Path $runs)) { return }
$cut = (Get-Date).AddHours(-$Hours)
$keep = { param($p) $p -match '\\runs\\(playtest|golden|user_reports)' } # (also the check-state reference sets and your bug-report pictures)
$old = Get-ChildItem $runs -Recurse -File -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -lt $cut -and -not (& $keep $_.FullName) }
$bytes = ($old | Measure-Object Length -Sum).Sum
if ($WhatIf) { "prune_runs: would delete {0} files, {1:N1} GB (older than {2} h)" -f $old.Count, ($bytes / 1GB), $Hours; return }
$old | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem $runs -Recurse -Directory -Force -ErrorAction SilentlyContinue |
    Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { -not (& $keep $_.FullName) -and -not (Get-ChildItem $_.FullName -Force -ErrorAction SilentlyContinue) } |
    Remove-Item -Force -ErrorAction SilentlyContinue
if ($old.Count) { "prune_runs: deleted {0} files, {1:N1} GB (older than {2} h)" -f $old.Count, ($bytes / 1GB), $Hours }
