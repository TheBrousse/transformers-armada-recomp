# install_layout.sh (TODO 279, docs/INSTALL_LAYOUT_PLAN.md) - the installed folder layout as the Linux scripts see it:
# tools/install_layout.ps1's functions. Sourced by build.sh, setup.sh, update.sh and tools/make_hd_textures.sh.
#
# An install is the folder the player picked (tfa-setup in it) holding the checkout. Layout 1: everything is in the
# checkout. Layout 2 (<install>/layout.json says {"layout": 2}): the player's things are in the install folder -
# settings.ini, saves/, logs/, data/game (the disc's files), data/movies_hd - and the checkout keeps the program, its
# build, the HD textures and the mods. Nothing here writes an absolute path anywhere: every place is found from where
# the checkout is (a copied install works in another folder or on another PC, section 2.4).

# The install folder above a game folder (the checkout, or its port/ in a repository that holds it), or nothing when
# the game folder is not in an install: the folder above with the setup program (tfa-setup; tfa-setup.exe in a copy
# from Windows), layout.json or the setup's .setup_unfinished mark in it. A checkout cloned and set up by hand has
# none of them above it: it is never taken for an install, and nothing of it is moved.
tfa_install_root() {
    local here above d
    here=$(cd "$1" 2>/dev/null && pwd) || return 0
    above=("$(dirname "$here")")
    if [ "$(basename "$here")" = port ]; then above+=("$(dirname "${above[0]}")"); fi
    # (.setup_unfinished: the setup's mark while it installs - it copies itself into the folder only at the end, so a
    # fresh install's first build would otherwise not see an install)
    for d in "${above[@]}"; do
        [ "$d" != "$here" ] || continue
        if [ -e "$d/tfa-setup" ] || [ -e "$d/tfa-setup.exe" ] || [ -e "$d/layout.json" ] || [ -e "$d/.setup_unfinished" ]; then
            printf '%s\n' "$d"
            return 0
        fi
    done
    return 0
}

# The layout number layout.json holds (1 without it), as the game reads it (install_layout.cpp: the number after
# "layout").
tfa_layout_version() {
    local n=""
    if [ -n "$1" ] && [ -f "$1/layout.json" ]; then
        n=$(grep -o '"layout"[[:space:]]*:[[:space:]]*[0-9]*' "$1/layout.json" 2>/dev/null | head -n 1 | grep -o '[0-9]*$' || true)
    fi
    printf '%s\n' "${n:-1}"
}

# The disc's extracted files: <install>/data/game in layout 2 when its executable is there, else <game folder>/game.
tfa_game_data_dir() {
    local root
    root=$(tfa_install_root "$1")
    if [ -n "$root" ] && [ "$(tfa_layout_version "$root")" = 2 ] &&
        { [ -f "$root/data/game/SLUS_206.68" ] || [ -f "$root/data/game/SLES_523.88" ]; }; then
        printf '%s\n' "$root/data/game"
    else
        printf '%s\n' "$(cd "$1" && pwd)/game"
    fi
}

# Where the scripts' own logs go: <install>/logs in layout 2 when it is there, else nothing (the caller's old place).
tfa_layout_logs_dir() {
    local root
    root=$(tfa_install_root "$1")
    if [ -n "$root" ] && [ "$(tfa_layout_version "$root")" = 2 ] && [ -d "$root/logs" ]; then printf '%s\n' "$root/logs"; fi
    return 0
}

# The install folder's settings.ini when it is the one in use (layout 2 and the file there), else nothing: then the
# checkout's settings.ini is the game's, made from the example on the first start.
tfa_layout_settings() {
    local root
    root=$(tfa_install_root "$1")
    if [ -n "$root" ] && [ "$(tfa_layout_version "$root")" = 2 ] && [ -f "$root/settings.ini" ]; then printf '%s\n' "$root/settings.ini"; fi
    return 0
}

# The move to layout 2 (tools/move_layout.py), in an install only and at the end of a good build, as build.ps1 starts
# tools\move_layout.ps1: it decides itself (once, or an interrupted move carried on) and never fails the build.
tfa_layout_move() {
    local root
    root=$(tfa_install_root "$1")
    [ -n "$root" ] || return 0
    "${PYTHON:-python3}" "$1/tools/move_layout.py" --game-folder "$1" --root "$root" || true
}
