#!/bin/sh
# Builds the Miyoo binary and assembles the SD card layout in dist/:
#   dist/Roms/PORTS/...                     copy to the SD card root
#   dist/scorched-emiyoolator-<version>.zip the same, zipped
#   scripts/package.sh [GAME_FOLDER]        (default: Original_EXE)
# The game folder (your copy of Scorched Earth 1.5) provides SCORCH.EXE and
# its data files; the native code is generated from that SCORCH.EXE.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$ROOT/VERSION")"
SRC="$(cd "${1:-$ROOT/Original_EXE}" && pwd)"
OUT="$ROOT/dist"
GAME="$OUT/Roms/PORTS/Games/ScorchedEarth"

[ -f "$SRC/SCORCH.EXE" ] || { echo "no SCORCH.EXE in $SRC" >&2; exit 1; }
make -C "$ROOT" gen EXE="$SRC/SCORCH.EXE" >/dev/null
make -C "$ROOT" -j"$(nproc)" host >/dev/null
"$ROOT/scripts/build-miyoo.sh"

rm -rf "$OUT/Roms" "$OUT"/scorched-emiyoolator-*.zip
mkdir -p "$OUT"
cp -R "$ROOT/packaging/Roms" "$OUT/Roms"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src gta1-miyoo-toolchain \
	arm-linux-gnueabihf-strip -o dist/Roms/PORTS/Games/ScorchedEarth/scorch build/miyoo/scorch
for f in "$SRC"/*; do
	case "$(basename "$f")" in
	SCORCH.CFG|SCORCH.MKT|*.SAV|uncompiled.txt) ;; # never ship settings or saves
	*) cp "$f" "$GAME/" ;;
	esac
done
echo "$VERSION" > "$GAME/VERSION"
mkdir -p "$OUT/Roms/PORTS/Imgs"
"$ROOT/scripts/make-art.sh" "$SRC" "$OUT/Roms/PORTS/Imgs/Scorched Earth.png"
chmod +x "$GAME/scorch" "$GAME/launch.sh" "$OUT/Roms/PORTS/Shortcuts/Strategy/Scorched Earth.port"
(cd "$OUT" && zip -qr "scorched-emiyoolator-$VERSION.zip" Roms)
echo "packaged $OUT/scorched-emiyoolator-$VERSION.zip"
