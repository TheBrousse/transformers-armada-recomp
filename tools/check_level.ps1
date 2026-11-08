<#
.SYNOPSIS
    Kept for old references: the in-level picture check is now the `level` case of
    tools\check_frames.ps1, which also checks the dropship walk, the Mini-Con calls, the
    Drop Zones room and the Mini-Con menu.

.EXAMPLE
    .\tools\check_level.ps1                 # same as .\tools\check_frames.ps1 -Case level
    .\tools\check_level.ps1 -Update
#>
param([switch]$Update)
& (Join-Path $PSScriptRoot "check_frames.ps1") -Case level -Update:$Update
