#!/usr/bin/env bash
# Plays the game (Linux: Play.bat's job). In the game: F1 = the menu (settings, saves), F11 = full screen.
# First start: settings.ini is made from settings.example.ini (overlay/settings.example.ini when there is one).
# For Steam: add this script as a non-Steam game.
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" || exit 1
# (TODO 279) The new folder layout, when the folder above is marked for it: settings.ini is the one up there (the game
# finds it, and the other places, itself). In the mods' repository this folder is its port/: the install folder is one
# more up. As Play.bat.
moved=0
if [ -f ../layout.json ] && [ -f ../settings.ini ]; then moved=1; fi
if [ "$(basename "$PWD")" = port ] && [ -f ../../layout.json ] && [ -f ../../settings.ini ]; then moved=1; fi
if [ "$moved" = 0 ] && [ ! -f settings.ini ]; then
    for example in overlay/settings.example.ini settings.example.ini; do
        [ -f "$example" ] && cp "$example" settings.ini && break
    done
fi
exec ./out/build/transformers/TransformersRecomp "$@"
