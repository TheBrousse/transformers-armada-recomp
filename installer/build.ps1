<#
.SYNOPSIS
    Builds the release tfa-setup.exe the way it is published (the setup workflow runs this).

.DESCRIPTION
    cargo build --release --locked, with:
      - the build machine's folders taken out of the exe: Rust keeps source paths for its panic
        messages, which would name this PC's user folder and Cargo's download cache
        (--remap-path-prefix: they read "cargo\..." and "tfa-setup\..." instead);
      - a deterministic exe: the linker's /Brepro writes a hash where the build time would go, so
        the same sources, Cargo.lock and toolchain give the same bytes.
    No packer, no compression: the exe is what the linker writes (strip = true in Cargo.toml).
    Prints the exe's SHA-256 at the end.

.EXAMPLE
    .\installer\build.ps1
#>
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE ".cargo" }
$flags = @(
    "--remap-path-prefix=$((Resolve-Path $cargoHome).Path)=cargo",
    "--remap-path-prefix=$((Resolve-Path $here).Path)=tfa-setup",
    "-Clink-arg=/Brepro"
)
# (CARGO_ENCODED_RUSTFLAGS: one flag per 0x1f-separated field, so folders with spaces stay whole)
$env:CARGO_ENCODED_RUSTFLAGS = $flags -join [char]0x1f
Remove-Item Env:RUSTFLAGS -ErrorAction SilentlyContinue
Push-Location $here
try {
    & cargo build --release --locked
    if ($LASTEXITCODE -ne 0) { throw "cargo build failed" }
} finally {
    Pop-Location
    Remove-Item Env:CARGO_ENCODED_RUSTFLAGS -ErrorAction SilentlyContinue
}
$exe = Join-Path $here "target\release\tfa-setup.exe"
Write-Host ("tfa-setup.exe  SHA-256 {0}" -f (Get-FileHash -Algorithm SHA256 $exe).Hash)
