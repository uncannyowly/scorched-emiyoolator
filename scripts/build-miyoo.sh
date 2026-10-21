#!/bin/sh
# Cross-compiles the Miyoo binary (build/miyoo/scorch) inside the toolchain
# container. The generated game code (build/gen) must exist: make recompile.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# the same image as the GTA1 port's, if it is already there
IMAGE=gta1-miyoo-toolchain

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	docker build -t "$IMAGE" "$ROOT"
fi
[ -f "$ROOT/build/gen/index.c" ] || make -C "$ROOT" recompile
[ -f "$ROOT/build/gen/vgafont.c" ] || make -C "$ROOT" build/gen/vgafont.c

docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src "$IMAGE" \
	make miyoo -j"$(nproc)"

file "$ROOT/build/miyoo/scorch" 2>/dev/null || true
