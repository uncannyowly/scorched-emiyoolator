#!/bin/sh
# Scorched Earth 1.5 (1995) for the Miyoo Mini / Mini Plus on OnionOS: the
# original SCORCH.EXE, recompiled to native ARM code. See README.txt.

GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$GAMEDIR" || exit 1

LOG="$GAMEDIR/scorch.log"
CPUCLOCK=/mnt/SDCARD/.tmp_update/bin/cpuclock
AUDIOSERVER=""

cleanup() {
	sync
	[ -x "$CPUCLOCK" ] && "$CPUCLOCK" 1200 >/dev/null 2>&1
	echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
	# give OnionOS its audio back
	if [ -n "$AUDIOSERVER" ] && ! pgrep audioserver >/dev/null 2>&1; then
		"$AUDIOSERVER" >/dev/null 2>&1 &
	fi
}
trap cleanup EXIT INT TERM HUP

if [ ! -f "$GAMEDIR/SCORCH.EXE" ]; then
	echo "SCORCH.EXE missing in $GAMEDIR" > "$LOG"
	exit 1
fi
# first start: settings for the handheld (320x240 fills the screen)
[ -f "$GAMEDIR/SCORCH.CFG" ] || cp "$GAMEDIR/scorch-miyoo.cfg" "$GAMEDIR/SCORCH.CFG"

# Mini Plus (Wi-Fi) runs at 1600 MHz; the original Mini stays at 1400 MHz
if [ -d /sys/class/net/wlan0 ] || grep -qi plus /proc/cmdline 2>/dev/null; then
	FREQ=1600
else
	FREQ=1400
fi
[ -x "$CPUCLOCK" ] && "$CPUCLOCK" $FREQ >/dev/null 2>&1

# the game drives the audio hardware itself (MI_AO)
for a in /mnt/SDCARD/.tmp_update/bin/audioserver /customer/app/audioserver; do
	[ -x "$a" ] && AUDIOSERVER="$a" && break
done
if pgrep audioserver >/dev/null 2>&1; then
	killall -9 audioserver 2>/dev/null
	sleep 0.2
else
	AUDIOSERVER=""
fi

unset LD_PRELOAD
export LD_LIBRARY_PATH="/config/lib:/customer/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"
chmod +x "$GAMEDIR/scorch" 2>/dev/null

{
	echo "=== Scorched Earth eMiYoolator $(cat "$GAMEDIR/VERSION" 2>/dev/null) started $(date) at ${FREQ} MHz"
	grep -E "MemAvailable" /proc/meminfo
	"$GAMEDIR/scorch" --dir "$GAMEDIR" "$@"
	echo "=== exit code $?"
} > "$LOG" 2>&1
