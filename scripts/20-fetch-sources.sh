#!/bin/bash
# Holt Kernel-Quellen und Maemo-Leste-Rootfs auf den Build-Host (x86_64).
set -euo pipefail
WORK=${WORK:?WORK=<build-verzeichnis> setzen}
LESTE_BUILD=${LESTE_BUILD:-20260426-daedalus}
LESTE_IMG=maemo-leste-5.0-arm64-arm64-generic-${LESTE_BUILD%%-*}.img.xz
LESTE_URL=https://maedevu.maemo.org/images/arm64-generic/$LESTE_BUILD

mkdir -p "$WORK/build" "$WORK/dl"

# Mainline-Kernel mit Pro1X-Support (Branch des sm6115-mainline-Projekts)
if [ ! -d "$WORK/build/linux" ]; then
  git clone --depth 1 -b qx1050-7.1 \
    https://github.com/sm6115-mainline/linux.git "$WORK/build/linux"
fi

cd "$WORK/dl"
[ -f "$LESTE_IMG" ] || curl -fL -O "$LESTE_URL/$LESTE_IMG"
curl -fL -O "$LESTE_URL/$LESTE_IMG.sha256"
sha256sum -c "$LESTE_IMG.sha256"
echo FETCH_DONE
