#!/bin/sh
# FreeBSD on the Pro1X: which way the touchscreen's coordinates lie, from
# four taps on the console, before X (tools/devday/fbsd-x.sh uses the
# result). As root:
#
#   sh /dev/pro1x-host/freebsd/touchcal.sh
#
# Holding the phone as for typing, tap near the corners in this order:
# top left, top right, bottom right, bottom left. The raw events of the
# Goodix touchscreen's evdev node (hexdump of struct input_event) give the
# first position of each tap; from them which raw axis runs along the
# screen's width and which way, and so the libinput transformation matrix
# (the device's normalised coordinates to the screen's). Written to
# /tmp/pro1x-xorg.conf.d/10-pro1x.conf, matched by the product name (the
# touchscreen class did not match: no BTN_TOUCH), and with the taps into
# /dev/pro1xshot for Linux to read after the next boot.
OUT=/tmp/pro1x-xorg.conf.d
EV=/tmp/pro1x-touchcal.ev
LOG=/tmp/pro1x-touchcal.txt
dev=
for n in $(sysctl -N kern.evdev.input 2> /dev/null | sed -n 's/^kern\.evdev\.input\.\([0-9]*\)\.name$/\1/p'); do
	case "$(sysctl -n kern.evdev.input.$n.name 2> /dev/null)" in
	*[Gg]oodix*) dev=/dev/input/event$n ;;
	esac
done
if [ -z "$dev" ]; then
	echo "no Goodix touchscreen among kern.evdev.input" | tee $LOG
	exit 1
fi
echo "Touchscreen: $dev. Tap near the corners, holding the phone as for"
echo "typing: TOP LEFT, TOP RIGHT, BOTTOM RIGHT, BOTTOM LEFT (25 s)"
# struct input_event: struct timeval (16 bytes), type, code, value
timeout 25 hexdump -v -e '4/4 "%u " 2/2 " %u" 1/4 " %d" "\n"' $dev > $EV 2> /dev/null
# The first X and Y after each new tracking id (ABS_MT_TRACKING_ID 57 >= 0)
awk '
$5 == 3 && $6 == 57 { if ($7 >= 0) { t++; gx[t] = ""; gy[t] = "" } next }
$5 == 3 && $6 == 53 { if (t && gx[t] == "") gx[t] = $7; next }
$5 == 3 && $6 == 54 { if (t && gy[t] == "") gy[t] = $7; next }
END {
	for (i = 1; i <= t; i++) printf "tap %d: x %s y %s\n", i, gx[i], gy[i]
	if (t < 4 || gx[1] == "" || gy[1] == "" || gx[2] == "" || gy[2] == "" || gx[4] == "" || gy[4] == "") {
		print "fewer than four taps with positions: no matrix"; exit 1
	}
	# along the width: tap 1 to 2; along the height: tap 1 to 4
	wx = gx[2] - gx[1]; wy = gy[2] - gy[1]
	hx = gx[4] - gx[1]; hy = gy[4] - gy[1]
	ax = (wx < 0 ? -wx : wx); ay = (wy < 0 ? -wy : wy)
	if (ax >= ay) {		# raw x along the width
		sw = (wx > 0 ? 1 : -1); sh = (hy > 0 ? 1 : -1)
		m = sprintf("%d 0 %d 0 %d %d 0 0 1", sw, sw < 0, sh, sh < 0)
	} else {		# raw y along the width: swapped
		sw = (wy > 0 ? 1 : -1); sh = (hx > 0 ? 1 : -1)
		m = sprintf("0 %d %d %d 0 %d 0 0 1", sw, sw < 0, sh, sh < 0)
	}
	print "matrix " m
}' $EV | tee $LOG
m=$(sed -n 's/^matrix //p' $LOG)
if [ -n "$m" ]; then
	mkdir -p $OUT
	cat > $OUT/10-pro1x.conf <<EOF
# From touchcal.sh's taps, $(date)
Section "Device"
	Identifier	"Framebuffer"
	Driver		"scfb"
	Option		"Rotate" "CW"
EndSection

Section "InputClass"
	Identifier	"Pro1X touchscreen"
	MatchProduct	"Goodix"
	Option		"TransformationMatrix" "$m"
EndSection
EOF
	echo "written: $OUT/10-pro1x.conf; now sh /dev/pro1x-host/freebsd/x.sh"
fi
{ cat $LOG; echo "--- events"; head -c 200000 $EV; } > /dev/pro1xshot
