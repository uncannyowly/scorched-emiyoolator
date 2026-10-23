#!/bin/sh
# Equivalence check: the recompiled build and the interpreter must produce
# byte-identical event traces (state at every interrupt delivery) for each
# test session in tests/, and the result must not depend on the frame rate.
# Usage: scripts/equivalence.sh [arm]   (arm: also run the Miyoo build under
# QEMU via docker --platform linux/arm/v7 and compare it with the PC)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
OUT=build/eq
mkdir -p "$OUT" build/shots

ms() { case $1 in cover) echo 90000;; mouse) echo 7000;; walkfire) echo 18500;; mod2) echo 21600;; rounds) echo 240000;; weapons) echo 150000;; esac; }
pad() { case $1 in cover) echo "";; *) echo "--pad";; esac; }
fresh() { # a clean game directory with the port's first-start settings
	rm -rf "$1"; mkdir -p "$1"
	cp Original_EXE/* "$1"/
	printf 'GRAPHICS_MODE=320x240\n' > "$1/SCORCH.CFG"
}
run() { # binary test name extra-args
	d=$OUT/run_$3; fresh "$d"
	./build/host/$1 --dir "$d" --headless $(pad $2) $4 --ms "$(ms $2)" --script tests/$2.txt \
		--trace-events "$OUT/$3.txt" --log "$OUT/$3.log"
}

make host interp >/dev/null || exit 1
for t in cover mouse walkfire mod2 rounds weapons; do
	run scorch $t c_$t "" &
	run scorch-interp $t i_$t "" &
	run scorch $t f30_$t "--fps 30" &
done
wait
fail=0
for t in cover mouse walkfire mod2 rounds weapons; do
	for v in c f30; do
		if cmp -s "$OUT/${v}_$t.txt" "$OUT/i_$t.txt"; then r=identical; else r=DIFFERENT; fail=1; fi
		echo "$t: $v vs interpreter: $r ($(wc -l < "$OUT/i_$t.txt") events)"
	done
	grep -h "^exit" "$OUT/c_$t.log"
done

if [ "${1:-}" = arm ]; then
	[ -x build/miyoo/scorch ] || scripts/build-miyoo.sh >/dev/null
	for t in cover walkfire mod2 rounds weapons; do
		d=$OUT/arm_$t; fresh "$d"
		sed 's#build/shots/#/game/#' tests/$t.txt > "$d/s.txt"
		docker run --rm --platform linux/arm/v7 -v "$ROOT/build/miyoo:/app:ro" -v "$ROOT/$d:/game" \
			debian:bookworm-slim /app/scorch --dir /game --headless $(pad $t) --ms "$(ms $t)" \
			--script /game/s.txt --log /game/log.txt --trace-events /game/ev.txt &
	done
	wait
	for t in cover walkfire mod2 rounds weapons; do
		if cmp -s "$OUT/arm_$t/ev.txt" "$OUT/i_$t.txt"; then r=identical; else r=DIFFERENT; fail=1; fi
		echo "$t: ARM vs PC interpreter: $r"
	done
fi
exit $fail
