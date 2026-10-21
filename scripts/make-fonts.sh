#!/bin/sh
# Dumps the VGA BIOS ROM fonts (8x8, 8x14, 8x16; the IBM VGA designs) from
# DOSBox into build/fonts.bin. Fastgraph, and so the game, prints all its
# text with these fonts. Needs dosbox (0.74) on the PATH.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/fonts.bin"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/dump.asm" <<'ASM'
org 100h
	mov ah, 3Ch
	xor cx, cx
	mov dx, fname
	int 21h
	mov [handle], ax
	mov bh, 3
	mov cx, 1024
	call dumpfont
	mov bh, 4
	mov cx, 1024
	call dumpfont
	mov bh, 2
	mov cx, 3584
	call dumpfont
	mov bh, 6
	mov cx, 4096
	call dumpfont
	mov ah, 3Eh
	mov bx, [handle]
	int 21h
	mov ax, 4C00h
	int 21h
dumpfont:
	push cx
	mov ax, 1130h
	int 10h
	pop cx
	push ds
	push es
	pop ds
	mov dx, bp
	mov bx, [cs:handle]
	mov ah, 40h
	int 21h
	pop ds
	ret
fname db 'FONTS.BIN', 0
handle dw 0
ASM
nasm -f bin -o "$WORK/DUMP.COM" "$WORK/dump.asm"
printf '[sdl]\noutput=surface\n[dosbox]\nmachine=svga_s3\n[mixer]\nnosound=true\n[speaker]\npcspeaker=false\n' > "$WORK/dosbox.conf"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 60 dosbox -conf "$WORK/dosbox.conf" \
	-c "mount c $WORK" -c "c:" -c "DUMP.COM" -c "exit" >/dev/null 2>&1
test "$(wc -c < "$WORK/FONTS.BIN")" -eq 9728
mkdir -p "$ROOT/build"
cp "$WORK/FONTS.BIN" "$OUT"
echo "wrote $OUT"
