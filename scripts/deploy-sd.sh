#!/bin/sh
# Installs dist/ (scripts/package.sh) on an OnionOS SD card:
#   scripts/deploy-sd.sh /run/media/$USER/CARD
# Keeps the card's SCORCH.CFG, SCORCH.MKT and saved games. Touches nothing
# outside Roms/PORTS/{Games/ScorchedEarth,Shortcuts/Strategy,Imgs}.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="${1:?SD card root}"
DIST="$ROOT/dist/Roms/PORTS"
DEST="$SD/Roms/PORTS/Games/ScorchedEarth"

[ -d "$SD/Roms/PORTS" ] || { echo "$SD does not look like an OnionOS card" >&2; exit 1; }
[ -x "$DIST/Games/ScorchedEarth/scorch" ] || { echo "run scripts/package.sh first" >&2; exit 1; }

mkdir -p "$DEST" "$SD/Roms/PORTS/Shortcuts/Strategy" "$SD/Roms/PORTS/Imgs"
for f in "$DIST/Games/ScorchedEarth"/*; do
	command cp -f "$f" "$DEST/"
done
command cp -f "$DIST/Shortcuts/Strategy/Scorched Earth.port" "$SD/Roms/PORTS/Shortcuts/Strategy/"
command cp -f "$DIST/Imgs/Scorched Earth.png" "$SD/Roms/PORTS/Imgs/"
sync
echo "installed $(cat "$DEST/VERSION") to $DEST"
ls -la "$DEST"
