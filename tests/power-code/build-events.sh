#!/bin/bash
# Builds pro1x-events on the PC for the wake-up measurements (gtk2, gconf,
# json-glib and libcurl from the PC; rtcom-stub.c in place of
# rtcom-eventlogger, its headers from freebsd/hildon's source tree; no
# calendar): $OUT/events-<rev> at real time and $OUT/events-<rev>-x15 with
# its update intervals divided by 15 (two hours in eight minutes).
#
#   tests/power-code/build-events.sh REV...     (e.g. 8f1c2a0~1 8f1c2a0)
#
# OUT (default /tmp/pro1x-power-code/bin), RTCOM_SRC (the source tree of
# rtcom-eventlogger, default $WORK/freebsd/hildon/src/rtcom-eventlogger).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$(cd "$REPO/.." && pwd)}
OUT=${OUT:-/tmp/pro1x-power-code/bin}
RTCOM_SRC=${RTCOM_SRC:-$WORK/freebsd/hildon/src/rtcom-eventlogger}
T=$(dirname "$OUT")
mkdir -p "$OUT" "$T/inc" "$T/src"
ln -sfn "$RTCOM_SRC/rtcom-eventlogger" "$T/inc/rtcom-eventlogger"
gcc -shared -fPIC -o "$OUT/librtcom-eventlogger.so" "$REPO/tests/power-code/rtcom-stub.c" \
  $(pkg-config --cflags --libs gobject-2.0)
for rev; do
  git -C "$REPO" show "$rev:tools/pro1x-events/pro1x-events.c" > "$T/src/events-$rev.c"
  # the intervals / 15, the old minute tick too (60 s -> 4 s)
  sed -e 's/^#define FEED_EVERY (15 \* 60)/#define FEED_EVERY 60/' \
      -e 's/^#define WEATHER_EVERY (30 \* 60)/#define WEATHER_EVERY 120/' \
      -e 's/^#define WEATHER_RETRY (5 \* 60)/#define WEATHER_RETRY 20/' \
      -e 's/^#define RETRY_FIRST (5 \* 60)/#define RETRY_FIRST 20/' \
      -e 's/^#define RETRY_MAX (60 \* 60)/#define RETRY_MAX 240/' \
      -e 's/^#define BATCH (5 \* 60)/#define BATCH 20/' \
      -e 's/g_timeout_add_seconds(60, feed_timer/g_timeout_add_seconds(4, feed_timer/' \
      "$T/src/events-$rev.c" > "$T/src/events-$rev-x15.c"
  for v in "$rev" "$rev-x15"; do
    # shellcheck disable=SC2046
    gcc -O2 -w -DPRO1X_NO_CALENDAR -I"$T/inc" -o "$OUT/events-$v" "$T/src/events-$v.c" \
      $(pkg-config --cflags --libs gtk+-2.0 gconf-2.0 json-glib-1.0 libcurl gio-2.0 x11) \
      -L"$OUT" -lrtcom-eventlogger -Wl,-rpath,"$OUT" -lm
  done
  echo ">> $OUT/events-$rev $OUT/events-$rev-x15"
done
