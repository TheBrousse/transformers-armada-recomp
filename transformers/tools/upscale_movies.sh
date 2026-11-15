#!/bin/bash
# Upscales every movie of the game (game/FMV: .PSS and .M2V) with Reforge, into movies_hd/ under the
# same relative paths (<name>.reforge.mkv), for the HD movies (src/hd_movies.cpp). Skips movies done.
#
# Usage: transformers/tools/upscale_movies.sh <Reforge folder> [<preset>] [<size WxH>]
#   Reforge folder: where Reforge.exe is (required)
#   preset: clean-upscale (faithful 2x, the default), old-dvd, anime-sd, ultra-sharp, ...
#   size: the output size (default 1800x1080: the movies are 5:3)
# Run from the recomp's root folder.

set -u
if [ $# -lt 1 ]; then
    echo "usage: transformers/tools/upscale_movies.sh <Reforge folder> [<preset>] [<size WxH>]" >&2
    exit 2
fi
REFORGE="$1"
PRESET="${2:-clean-upscale}"
SIZE="${3:-1800x1080}"
ROOT="$(pwd)"
export REFORGE_MODELS_DIR="$REFORGE/models"

cd "$ROOT/game/FMV" || exit 1
find . -type f \( -iname '*.pss' -o -iname '*.m2v' \) | sort | while read -r f; do
    rel="${f#./}"
    dir="$(dirname "$rel")"
    stem="$(basename "$rel")"
    stem="${stem%.*}"
    out="$ROOT/movies_hd/$dir"
    if ls "$out/$stem".reforge*.mkv >/dev/null 2>&1; then
        echo "done already: $rel"
        continue
    fi
    mkdir -p "$out"
    echo "== $rel"
    (cd "$REFORGE" && ./python/python.exe -m reforge run --preset "$PRESET" --size "$SIZE" --out "$(cygpath -w "$out")" \
        "$(cygpath -w "$ROOT/game/FMV/$rel")") 2>&1 | grep -E "wrote|ERROR|error" || true
done
echo "all movies done"
