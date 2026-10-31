#!/bin/bash
# Host tests of the sensor stack's libssc (docs/sensors.md), on the PC, no
# phone and no sensor core: libssc v0.4.4 with the three patches of
# scripts/46-sensor-stack.sh built with meson (as on the phone, against the
# PC's libqmi), then its requests and report parsing checked against the
# sensor core's field numbers (see-client.proto, check.py), the FreeBSD
# copies of the patches against Leste's, and pro1x-steps compiled.
#
#   bash tests/sensors/run.sh
#   LIBSSC_SRC=dir  a libssc clone to take v0.4.4 from (default: codeberg)
#   KEEP=dir        build there and keep it (default: a temporary directory)
#
# Needs git, meson, ninja, cc, pkg-config, protoc, protoc-c (protobuf-c),
# libqmi-glib >= 1.33.4, gio-unix-2.0 and python3 with protobuf.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
LIBSSC_REV=0cf77b93b55752da34dca2dcecc06fca8665184b	# v0.4.4
PATCHES="libssc-pedometer libssc-sync-wait libssc-wakeup-batching"
if [ -n "${KEEP:-}" ]; then
	T=$KEEP
	mkdir -p "$T"
else
	T=$(mktemp -d /tmp/pro1x-sensors-test.XXXXXX)
	trap 'rm -rf "$T"' EXIT
fi
fails=0
log() { echo ">> $*"; }
ok() { echo "ok   $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }

log "libssc v0.4.4 with $PATCHES"
src=$T/libssc
rm -rf "$src"
git clone -q "${LIBSSC_SRC:-https://codeberg.org/DylanVanAssche/libssc.git}" "$src"
git -c advice.detachedHead=false -C "$src" checkout -q -f "$LIBSSC_REV"
for p in $PATCHES; do
	git -C "$src" apply --whitespace=nowarn "$REPO/patches/$p.patch"
done
meson setup "$src/build" "$src" -Dprefix=/usr > "$T/meson.log" 2>&1 ||
	{ tail -20 "$T/meson.log"; exit 1; }
if ninja -C "$src/build" > "$T/ninja.log" 2>&1; then
	ok "libssc builds with the patches"
else
	grep -m20 -E "error|Error" "$T/ninja.log"
	fail "libssc builds with the patches"
	exit 1
fi
if grep -q "warning:" "$T/ninja.log" && grep "warning:" "$T/ninja.log" | grep -qv "gir:"; then
	grep "warning:" "$T/ninja.log" | grep -v "gir:" | head
	fail "no compiler warnings"
else
	ok "no compiler warnings"
fi

log "FreeBSD's copies"
for p in $PATCHES; do
	f=$REPO/freebsd/hildon/patches/libssc/$p.patch
	# the FreeBSD copy may start with a line of its own before the diff
	if [ -f "$f" ] && diff -q <(sed -n '/^diff --git/,$p' "$REPO/patches/$p.patch") \
		<(sed -n '/^diff --git/,$p' "$f") > /dev/null; then
		ok "freebsd/hildon/patches/libssc/$p.patch is Leste's"
	else
		fail "freebsd/hildon/patches/libssc/$p.patch is Leste's"
	fi
done

log "requests and reports"
# shellcheck disable=SC2046
cc -O2 -Wall -Werror -DG_LOG_DOMAIN='"libssc"' -I "$src/build/data" -I "$src/src" \
	-o "$T/encode" "$REPO/tests/sensors/encode.c" "$src/src/libssc-request.c" \
	"$src/build/data/ssc-common.pb-c.c" $(pkg-config --cflags --libs glib-2.0 libprotobuf-c)
protoc --python_out="$T" -I "$REPO/tests/sensors" "$REPO/tests/sensors/see-client.proto"
python3 -I "$REPO/tests/sensors/check.py" "$T" || fails=$((fails + 1))

log "pro1x-steps against it"
# shellcheck disable=SC2046
if cc -O2 -Wall -Werror -I "$src/src" -o "$T/pro1x-steps" "$REPO/tools/pro1x-steps/pro1x-steps.c" \
	-L "$src/build/src" -lssc $(pkg-config --cflags --libs gio-unix-2.0 qmi-glib) -lm 2> "$T/steps.log"; then
	ok "pro1x-steps compiles without warnings"
else
	head -20 "$T/steps.log"
	fail "pro1x-steps compiles without warnings"
fi
if cc -O2 -Wall -Werror -I "$src/src" -o "$T/n9-sensors" "$REPO/meego/sensors/n9-sensors.c" \
	-L "$src/build/src" -lssc $(pkg-config --cflags --libs gio-unix-2.0 qmi-glib) -lm 2> "$T/n9.log"; then
	ok "n9-sensors compiles without warnings"
else
	head -20 "$T/n9.log"
	fail "n9-sensors compiles without warnings"
fi

if [ "$fails" = 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
