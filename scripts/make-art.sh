#!/bin/sh
# Makes OnionOS's 256x256 Ports box art from the game's own title screen:
# runs the desktop build headless in 640x480 and crops the mountain.
#   scripts/make-art.sh GAME_FOLDER OUT.png
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$(cd "$1" && pwd)"
OUT="$2"
BIN="$ROOT/build/host/scorch"
[ -x "$BIN" ] || BIN="$ROOT/build/host/scorch-interp"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cp "$SRC"/SCORCH.EXE "$SRC"/*.MTN "$SRC"/TALK*.CFG "$WORK"/
printf 'GRAPHICS_MODE=640x480\n' > "$WORK/SCORCH.CFG"
# the game draws its own pointer from mouse motion: push it into the corner
printf '3000 mickeys 4000 4000\n4000 shot %s\n' "$WORK/title.ppm" > "$WORK/art.txt"
"$BIN" --dir "$WORK" --headless --ms 4200 --script "$WORK/art.txt" --log "$WORK/log" >/dev/null 2>&1
magick "$WORK/title.ppm" -crop 434x434+155+8 +repage -resize 256x256 "$OUT"
echo "wrote $OUT"
