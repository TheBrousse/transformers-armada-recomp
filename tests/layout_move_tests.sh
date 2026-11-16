#!/usr/bin/env bash
# layout_move_tests.sh - tools/move_layout.py and tools/install_layout.sh (TODO 279, the Linux side) on made-up installs
# in a temporary folder, as tests/layout_move_tests.ps1 does on Windows: the move itself, a second run (nothing more
# happens), a move stopped half-way and carried on, a move killed at each of its steps and run again (the same end as
# one that was never stopped), a save made between a kill and the run that carries on, the game running (nothing
# moves), an install with no saves yet (a fresh one), the setup's first build, a European disc, a checkout set up by
# hand (not an install: never moved), a settings.ini already in the install folder (merged), the mods' repository (the
# game folder is its port/), a moved install copied to another folder (every place still found), and play.sh in both
# layouts.
# Run: bash tests/layout_move_tests.sh   (exit code 0: all passed). Needs bash, python3 and coreutils, nothing else.
set -uo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
move=$here/tools/move_layout.py
. "$here/tools/install_layout.sh"
base=$(mktemp -d "${TMPDIR:-/tmp}/tfa_layout.XXXXXX")
trap 'rm -rf "$base"' EXIT
failed=0
check() { if eval "$1"; then echo "  ok    $2"; else echo "  FAIL  $2"; failed=$((failed + 1)); fi; }
read_file() { if [ -f "$1" ]; then cat "$1"; else echo "<missing>"; fi; }
run_move() { python3 "$move" --game-folder "$1" --root "$2" >/dev/null 2>&1; }

# An install as the setup program leaves it: <install>/tfa-setup and the checkout with the player's things in it.
# new_install <name> [no-saves] [elf=<name>] [no-setup] [mods-repo]; sets inst and co.
new_install() {
    local name=$1 saves=1 elf=SLUS_206.68 setup=1 mods=0 a
    shift
    for a in "$@"; do
        case "$a" in no-saves) saves=0 ;; elf=*) elf=${a#elf=} ;; no-setup) setup=0 ;; mods-repo) mods=1 ;; esac
    done
    inst=$base/$name
    if [ "$mods" = 1 ]; then co=$inst/transformers-armada-ps2/port; else co=$inst/transformers-armada-recomp; fi
    mkdir -p "$co/game/FMV" "$co/states" "$co/runs" "$co/out/build/transformers"
    [ "$setup" = 0 ] || echo setup >"$inst/tfa-setup"
    echo elf >"$co/game/$elf"; head -c 5000 /dev/zero | tr '\0' x >"$co/game/TFA.ZIP"; echo pss >"$co/game/FMV/A.PSS"
    echo "# example" >"$co/settings.example.ini"
    if [ "$saves" = 1 ]; then
        mkdir -p "$co/game/mc0/BASLUS-20668" "$co/game/mc1" "$co/Saves/Backups" "$co/Saves/Exports" "$co/movies_hd"
        echo "my save" >"$co/game/mc0/BASLUS-20668/save.bin"; echo "card 2" >"$co/game/mc1/x.bin"
        # (a real card folder holds several files, with spaces in their names: BASLUS-20668 TRANSFM, Icon.ico, icon.sys)
        mkdir -p "$co/game/mc0/BASLUS-20668 TRANSFM"
        echo "save data" >"$co/game/mc0/BASLUS-20668 TRANSFM/BASLUS-20668 TRANSFM"; echo icon >"$co/game/mc0/BASLUS-20668 TRANSFM/Icon.ico"
        echo "TF_FOV=1.4" >"$co/settings.ini"; echo quick >"$co/quick.ps2state"
        echo named >"$co/states/named.ps2state"; echo ok >"$co/states/update_result.txt"; : >"$co/states/.play_after_update"
        echo backup >"$co/Saves/Backups/b.bin"; echo export >"$co/Saves/Exports/e.psu"; echo "old readme" >"$co/Saves/README.txt"
        echo movie >"$co/movies_hd/N1.mp4"; echo log >"$co/runs/play_2026-11-11_1200.err"
    fi
}
# Every file under a folder with its contents' hash - the journal, the dated backup folder's name and its note (which
# names the date) left out: two moves made at different times compare equal when they left the same things.
tree_of() {
    (cd "$1" && find . -type f ! -name layout_move.json -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum |
        sed -E 's#before_layout2_[0-9_-]+#before_layout2_X#; s#^[0-9a-f]+ ( \./saves/backups/before_layout2_X/README\.txt)$#-\1#')
}

echo "1. a full move"
new_install "full install" # (a space in the name, as the game's own folder may have)
run_move "$co" "$inst"
r=$inst
check '[ "$(tfa_layout_version "$r")" = 2 ]' "layout.json says 2"
check '[ "$(read_file "$r/layout.json")" = "{\"layout\": 2}" ]' "layout.json holds only the number (no path)"
check '[ "$(read_file "$r/saves/mc0/BASLUS-20668/save.bin")" = "my save" ] && [ "$(read_file "$r/saves/mc1/x.bin")" = "card 2" ]' "memory cards in saves/mc0, saves/mc1"
check '[ "$(read_file "$r/settings.ini")" = "TF_FOV=1.4" ]' "settings.ini in the install folder"
check '[ "$(read_file "$r/saves/mc0/BASLUS-20668 TRANSFM/BASLUS-20668 TRANSFM")" = "save data" ] && [ "$(read_file "$r/saves/mc0/BASLUS-20668 TRANSFM/Icon.ico")" = icon ]' "a card folder with several files and spaces in its names"
check '[ "$(read_file "$r/saves/states/quick.ps2state")" = quick ] && [ "$(read_file "$r/saves/states/named.ps2state")" = named ]' "save states in saves/states"
check '[ "$(read_file "$co/states/update_result.txt")" = ok ] && [ ! -e "$r/saves/states/update_result.txt" ] && [ ! -e "$r/saves/states/.play_after_update" ]' "the updater's own files stay in the checkout"
check '[ "$(read_file "$r/saves/backups/b.bin")" = backup ] && [ "$(read_file "$r/saves/exports/e.psu")" = export ]' "F1 > Saves' folders in saves/"
check '[ "$(read_file "$r/data/game/SLUS_206.68")" = elf ] && [ ! -e "$co/game" ]' "the disc's files moved to data/game"
check '[ "$(read_file "$r/data/game/mc0.moved/BASLUS-20668/save.bin")" = "my save" ] && [ ! -e "$r/data/game/mc0" ]' "old cards renamed mc0.moved inside data/game"
check '[ "$(read_file "$r/data/movies_hd/N1.mp4")" = movie ]' "HD movies moved to data/movies_hd"
check '[ "$(read_file "$r/logs/play_2026-11-11_1200.err")" = log ]' "game logs copied to logs/"
bk=$(find "$r/saves/backups" -maxdepth 1 -type d -name 'before_layout2_*')
check '[ "$(printf "%s\n" "$bk" | grep -c .)" = 1 ] && [ "$(read_file "$bk/game/mc0/BASLUS-20668/save.bin")" = "my save" ] && [ "$(read_file "$bk/settings.ini")" = "TF_FOV=1.4" ]' "the backup holds the cards and the settings"
check '[ "$(read_file "$co/settings.ini")" = "TF_FOV=1.4" ] && [ "$(read_file "$co/quick.ps2state")" = quick ]' "the originals in the checkout are left as they were"
check '[ "$(tfa_game_data_dir "$co")" = "$r/data/game" ]' "the scripts find the disc's files (tfa_game_data_dir)"
check '[ "$(tfa_layout_logs_dir "$co")" = "$r/logs" ] && [ "$(tfa_layout_settings "$co")" = "$r/settings.ini" ]' "the scripts' logs go to logs/, the install folder's settings.ini is the one in use"
check 'grep -q "\"finished\"" "$r/layout_move.json" && ! grep -qF "$r" "$r/layout_move.json"' "the journal is finished and holds no absolute path"
check '[ ! -e "$r/.layout_move_tmp" ]' "no temporary files left"
reference=$(tree_of "$r")

echo "2. run again: nothing more happens"
echo "newer save" >"$r/saves/mc0/BASLUS-20668/save.bin"
run_move "$co" "$inst"
check '[ "$(read_file "$r/saves/mc0/BASLUS-20668/save.bin")" = "newer save" ] && [ "$(find "$r/saves/backups" -maxdepth 1 -type d -name "before_layout2_*" | grep -c .)" = 1 ]' "a second run changes nothing"
echo "my save" >"$r/saves/mc0/BASLUS-20668/save.bin"

echo "3. stopped half-way (the disc's folder could not be moved), then carried on"
new_install stopped
r=$inst
mkdir -p "$r/data/game"; echo x >"$r/data/game/blocker" # (a folder in the way: the move stops there)
run_move "$co" "$inst"
check '[ "$(tfa_layout_version "$r")" = 2 ] && [ "$(read_file "$r/saves/mc0/BASLUS-20668/save.bin")" = "my save" ]' "saves and settings in place, layout.json written"
check '[ "$(tfa_game_data_dir "$co")" = "$co/game" ]' "the disc's files still found in the checkout (each place used only once it is there)"
rm -rf "$r/data/game"
run_move "$co" "$inst"
check '[ "$(read_file "$r/data/game/SLUS_206.68")" = elf ] && [ "$(tfa_game_data_dir "$co")" = "$r/data/game" ]' "the next run finishes the move"
check 'grep -q "\"finished\"" "$r/layout_move.json"' "the journal is finished"

echo "4. killed at each step (as a PC going off in the middle of an update), then run again"
for point in journal backup copies layout moves; do
    new_install "killed at $point"
    TF_LAYOUT_TEST_STOP=$point python3 "$move" --game-folder "$co" --root "$inst" >/dev/null 2>&1
    code=$?
    case "$point" in
    journal | backup | copies)
        check '[ "$code" = 9 ] && [ ! -e "$inst/layout.json" ] && [ "$(tfa_game_data_dir "$co")" = "$co/game" ] && [ "$(read_file "$co/game/mc0/BASLUS-20668/save.bin")" = "my save" ]' \
            "killed after the $point step: the old places still in use, and whole" ;;
    *)
        check '[ "$code" = 9 ] && [ "$(tfa_layout_version "$inst")" = 2 ] && [ "$(read_file "$inst/saves/mc0/BASLUS-20668/save.bin")" = "my save" ] && [ -f "$(tfa_game_data_dir "$co")/SLUS_206.68" ]' \
            "killed after the $point step: the new places in use, the disc's files found where they are" ;;
    esac
    run_move "$co" "$inst"
    check '[ "$(tree_of "$inst")" = "$reference" ] && grep -q "\"finished\"" "$inst/layout_move.json"' "run again after the $point step: the same end as a move never stopped"
done

echo "5. a save made between a kill and the run that carries on (the game still used the old places)"
new_install "saved between"
TF_LAYOUT_TEST_STOP=copies python3 "$move" --game-folder "$co" --root "$inst" >/dev/null 2>&1
echo "a newer save" >"$co/game/mc0/BASLUS-20668/save.bin"; echo "a new state" >"$co/states/later.ps2state"
run_move "$co" "$inst"
check '[ "$(read_file "$inst/saves/mc0/BASLUS-20668/save.bin")" = "a newer save" ] && [ "$(read_file "$inst/saves/states/later.ps2state")" = "a new state" ]' "the newer save and the new state are the ones in the new places"

echo "6. the game running: nothing moves"
new_install running
# (a copy of bash under the game's name, in the game folder as the game makes its folder the current one; the loop
# keeps that process itself running, where one command alone would be exec'd in its place)
cp "$(command -v bash)" "$co/out/build/transformers/TransformersRecomp"
(cd "$co" && exec ./out/build/transformers/TransformersRecomp -c 'while :; do sleep 0.2; done') &
game=$!
sleep 0.3
run_move "$co" "$inst"
check '[ ! -e "$inst/layout.json" ] && [ ! -e "$inst/layout_move.json" ] && [ -f "$co/game/SLUS_206.68" ]' "the game from this checkout running: nothing touched"
kill "$game" 2>/dev/null; wait "$game" 2>/dev/null
run_move "$co" "$inst"
check '[ "$(tfa_layout_version "$inst")" = 2 ]' "closed: the next run moves"

echo "7. other installs"
new_install "no saves yet" no-saves
run_move "$co" "$inst"
check '[ "$(tfa_layout_version "$inst")" = 2 ] && [ -d "$inst/saves/mc0" ] && [ "$(read_file "$inst/data/game/SLUS_206.68")" = elf ]' "a fresh install: saves/mc0 made, so new saves land there"
check '[ "$(read_file "$inst/settings.ini")" = "# example" ] && [ ! -e "$co/settings.ini" ]' "a fresh install: settings.ini made from the example in the install folder (none in the checkout)"
new_install "setup running" no-setup no-saves
: >"$inst/.setup_unfinished"
check '[ "$(tfa_install_root "$co")" = "$inst" ]' "the setup's first build (only its .setup_unfinished mark there yet): an install, moved at once"
new_install pal elf=SLES_523.88
run_move "$co" "$inst"
check '[ "$(tfa_game_data_dir "$co")" = "$inst/data/game" ]' "the European disc: data/game found by its own executable"
new_install "by hand" no-setup
before=$(tree_of "$inst")
tfa_layout_move "$co" >/dev/null 2>&1 # (what build.sh runs after its build)
check '[ -z "$(tfa_install_root "$co")" ] && [ "$(tree_of "$inst")" = "$before" ] && [ "$(tfa_game_data_dir "$co")" = "$co/game" ]' "a checkout set up by hand (no tfa-setup above it): not an install, nothing moved by a build"
new_install "from windows" no-setup
echo setup >"$inst/tfa-setup.exe"
check '[ "$(tfa_install_root "$co")" = "$inst" ]' "an install copied from Windows (tfa-setup.exe above): an install"

echo "8. a settings.ini already in the install folder: merged"
new_install "install settings"
printf '# my settings\nTF_FOV=1.4\nTF_DRAW_DISTANCE=3\nPS2X_FULLSCREEN=0\n' >"$co/settings.ini"
printf 'PS2X_FULLSCREEN=1\nTF_FPS_CAP=120\n' >"$inst/settings.ini"
run_move "$co" "$inst"
check 'grep -qx "TF_FOV=1.4" "$inst/settings.ini" && grep -qx "TF_DRAW_DISTANCE=3" "$inst/settings.ini" && grep -qx "# my settings" "$inst/settings.ini"' "the player's own settings and comments kept"
check 'grep -qx "PS2X_FULLSCREEN=1" "$inst/settings.ini" && ! grep -qx "PS2X_FULLSCREEN=0" "$inst/settings.ini" && grep -qx "TF_FPS_CAP=120" "$inst/settings.ini"' "the install folder's values win, its new keys added"
bk=$(find "$inst/saves/backups" -maxdepth 1 -type d -name 'before_layout2_*')
check 'grep -q PS2X_FULLSCREEN=1 "$bk/settings_install_folder.ini" && grep -q TF_DRAW_DISTANCE=3 "$bk/settings.ini"' "both files as they were in the backup"

echo "9. the mods' repository: the game folder is its port/"
new_install "mods install" mods-repo
mkdir -p "$co/Mods/JetBike"; echo off >"$co/Mods/JetBike/disabled"
run_move "$co" "$inst"
check '[ "$(tfa_install_root "$co")" = "$inst" ] && [ "$(tfa_layout_version "$inst")" = 2 ]' "the install found above the repository"
check '[ "$(read_file "$inst/saves/mc0/BASLUS-20668/save.bin")" = "my save" ] && [ "$(tfa_game_data_dir "$co")" = "$inst/data/game" ]' "cards in saves/, the disc's files in data/game"
check '[ "$(read_file "$co/Mods/JetBike/disabled")" = off ]' "Mods/ and the mods' on/off choices stay in port/"
bk=$(find "$inst/saves/backups" -maxdepth 1 -type d -name 'before_layout2_*')
check 'grep -qF "transformers-armada-ps2/port" "$bk/README.txt"' "the backup's note names the real folder"

echo "10. a moved install copied whole to another folder (another drive, a Steam Deck)"
cp -a "$base/full install" "$base/copied elsewhere"
c="$base/copied elsewhere" co2="$base/copied elsewhere/transformers-armada-recomp"
check '[ "$(tfa_install_root "$co2")" = "$c" ] && [ "$(tfa_game_data_dir "$co2")" = "$c/data/game" ]' "every place found from the copy's own folder"
check '! grep -rqF "$base/full install" "$c" --include="*.json" --include="*.ini" --include="*.txt"' "no file of the copy names the old folder"

echo "11. play.sh in both layouts (the game started with no settings.ini made in the checkout once moved)"
for kind in moved "by hand"; do
    if [ "$kind" = moved ]; then new_install "play moved" no-saves; run_move "$co" "$inst"; else new_install "play by hand" no-setup no-saves; fi
    cp "$here/play.sh" "$co/play.sh"
    printf '#!/bin/sh\npwd >"%s/started_in"\n' "$co" >"$co/out/build/transformers/TransformersRecomp"
    chmod +x "$co/play.sh" "$co/out/build/transformers/TransformersRecomp"
    "$co/play.sh"
    if [ "$kind" = moved ]; then
        check '[ "$(read_file "$co/started_in")" = "$co" ] && [ ! -e "$co/settings.ini" ] && [ -f "$inst/settings.ini" ]' "moved: the game started in the checkout, no settings.ini made there"
    else
        check '[ "$(read_file "$co/started_in")" = "$co" ] && [ "$(read_file "$co/settings.ini")" = "# example" ]' "by hand: settings.ini made from the example in the checkout, as before"
    fi
done

if [ "$failed" -gt 0 ]; then printf '\n%s check(s) failed\n' "$failed"; exit 1; fi
printf '\nall checks passed\n'
exit 0
