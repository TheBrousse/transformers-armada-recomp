<#
.SYNOPSIS
    Checks the native pixel path (PS2X_GPU_NATIVE=1) against the emulated GPU renderer: plays
    tools\check_frames.ps1's cases with each, keeps both sets of frames and measures how far
    apart they are with tools\native_compare.py (the native path is not meant to be exact).

.DESCRIPTION
    Both runs draw with the GPU alone (PS2X_GPU_MIRROR=0: its frame is then runs\shot_<tick>.png)
    at -Scale, with VIF1 on the EE thread (PS2X_MTVU=0) so that both play exactly the same game.
    The reference (PS2X_GPU_NATIVE=0) is kept in runs\native_ref\<case>\, the candidate (the
    same with -Set on top) in runs\native_new\<case>\. check_frames copies whatever runs\ holds
    for a tick - a shot_<tick>_gpu.png left by an earlier run with the mirror on, or an old frame
    where this run did not get that far - so the frames a run did not write are removed again.

    The game must not be running (the runs would start a second one beside it): the script
    stops when TransformersRecomp.exe is in the task list. -CompareOnly compares the frames
    already kept without running anything; -KeepReference runs only the candidate.

    native_compare.py's exit code is left in $LASTEXITCODE: 1 when a case is outside -Limits,
    2 when nothing could be compared.

.EXAMPLE
    .\tools\check_native.ps1                                    # all five cases at scale 3
    .\tools\check_native.ps1 -Case dropship,room -Scale 1
    .\tools\check_native.ps1 -KeepReference -Limits @{ "max-mean" = 1; "max-flat16" = 0.05; "min-ssim" = 0.98 }
    .\tools\check_native.ps1 -CompareOnly -Heatmaps runs\native_heat
#>
param(
    [string[]]$Case = @("level", "dropship", "minicon", "room", "menu"),
    [string]$Scale = "3",                           # PS2X_GPU_SCALE of both runs: <n>, or <x>x<y> across and down
    [hashtable]$Set = @{ PS2X_GPU_NATIVE = "1" },   # the candidate's environment variables, over the reference's
    [string]$Exe = "",                              # another build to run (e.g. out\play\TransformersRecomp.exe), else out\build's
    [string]$Reference = "runs\native_ref",         # the emulated renderer's frames (a folder per case)
    [string]$Candidate = "runs\native_new",         # the candidate's
    [switch]$CompareOnly,                           # compare the frames already kept, without running the game
    [switch]$KeepReference,                         # run the candidate only, against the reference kept before
    [hashtable]$Limits = @{},                       # native_compare.py's limits, e.g. @{ "max-mean" = 1; "max-flat16" = 0.05 }
    [string]$Heatmaps = ""                          # native_compare.py --heatmaps: a folder for the difference pictures
)
$Root = Split-Path $PSScriptRoot -Parent
if (-not $CompareOnly) {
    $images = @("TransformersRecomp.exe")
    if ($Exe) { $images += Split-Path $Exe -Leaf }
    foreach ($image in ($images | Select-Object -Unique)) {
        if (tasklist /FI "IMAGENAME eq $image" /NH | Select-String -SimpleMatch $image) {
            throw "$image is running: close it first (or compare the frames already kept with -CompareOnly)"
        }
    }
}
Push-Location $Root
try {
    if (-not $CompareOnly) {
        # PS2X_GPU=1 too, in case the shell has the software renderer chosen.
        $referenceSet = @{ PS2X_GPU = "1"; PS2X_GPU_MIRROR = "0"; PS2X_MTVU = "0"; PS2X_GPU_SCALE = $Scale; PS2X_GPU_NATIVE = "0"
                           PS2X_GPU_FULL_FIELDS = "0" } # (the candidate the same but for -Set: full fields only where it asks)
        $candidateSet = $referenceSet.Clone()
        foreach ($key in $Set.Keys) { $candidateSet[$key] = "$($Set[$key])" }
        $sides = @()
        if (-not $KeepReference) { $sides += @{ Name = "reference"; Folder = $Reference; Set = $referenceSet } }
        $sides += @{ Name = "candidate"; Folder = $Candidate; Set = $candidateSet }
        foreach ($side in $sides) {
            "== $($side.Name) -> $($side.Folder): " + (($side.Set.GetEnumerator() | Sort-Object Key | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join " ")
            $started = Get-Date
            $arguments = @{ Case = $Case; Set = $side.Set; GoldenRoot = $side.Folder; Update = $true }
            if ($Exe) { $arguments.Exe = $Exe }
            & .\tools\check_frames.ps1 @arguments
            # (a copy keeps its file's time: older than the run, it is not this run's frame; the
            # shot_<tick>_gpu.png ones are always an earlier run's, the others mean a tick not reached)
            foreach ($name in $Case) {
                $stale = @(Get-ChildItem (Join-Path $side.Folder $name) -Filter *.png -File -ErrorAction SilentlyContinue |
                           Where-Object { $_.LastWriteTime -lt $started })
                if ($stale.Count) { $stale | Remove-Item -Force }
                $missed = @($stale | Where-Object { $_.Name -notlike "*_gpu.png" })
                if ($missed.Count) { "${name}: removed $($missed.Count) frame(s) this run did not write: $(($missed | ForEach-Object Name) -join ', ')" }
            }
        }
    }
    $arguments = @("tools\native_compare.py", $Reference, $Candidate, "--case", ($Case -join ","))
    foreach ($key in $Limits.Keys) {
        $option = "--" + ("$key" -replace "^-+", "" -replace "_", "-")
        $arguments += @($option, "$($Limits[$key])")
    }
    if ($Heatmaps) { $arguments += @("--heatmaps", $Heatmaps) }
    python @arguments
}
finally { Pop-Location }
