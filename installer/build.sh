#!/usr/bin/env bash
# Builds the release tfa-setup for Linux (installer/build.ps1's job for tfa-setup.exe): cargo build --release, with the
# build machine's folders taken out of the program (Rust keeps source paths for its panic messages: they read
# "cargo/..." and "tfa-setup/..." instead). Prints the program's SHA-256 at the end.
#
# Build it on an older distribution (Ubuntu 22.04, glibc 2.35: a container is fine) and it runs on the newer ones -
# SteamOS, Bazzite, Fedora, Arch, Ubuntu 24.04. It needs nothing installed to run: X11 or Wayland, OpenGL and the
# desktop's file dialog (kdialog or zenity) are found when it starts.
#
#   ./installer/build.sh
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cargo_home=${CARGO_HOME:-$HOME/.cargo}
# (CARGO_ENCODED_RUSTFLAGS: one flag per 0x1f-separated field, so folders with spaces stay whole)
CARGO_ENCODED_RUSTFLAGS=$(printf '%s\x1f%s' "--remap-path-prefix=$cargo_home=cargo" "--remap-path-prefix=$here=tfa-setup")
export CARGO_ENCODED_RUSTFLAGS
unset RUSTFLAGS
cd "$here"
cargo build --release --locked
exe=$here/target/release/tfa-setup
printf 'tfa-setup  SHA-256 %s\n' "$(sha256sum "$exe" | cut -d' ' -f1 | tr a-f A-F)"
