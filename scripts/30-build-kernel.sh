#!/bin/bash
# Baut den Mainline-Kernel 7.1 mit den Pro1X-Gerätepatches (Build-Host, x86_64).
#
# Der Branch qx1050-7.1 von sm6115-mainline ist identisch mit Upstream v7.1;
# der komplette Gerätesupport steckt in den Patches des NixOS-Ports, die hier
# in genau dessen Reihenfolge angewendet werden (spätere Patches editieren
# denselben Device Tree).
set -euo pipefail
WORK=${WORK:?WORK=<build-verzeichnis> setzen}
LINUX=$WORK/build/linux
NIXOS=$WORK/build/nixos-pro1x
OUT=$WORK/out
JOBS=${JOBS:-$(nproc)}

PATCHES=(touch egpio dsi panelgap dpugap audio q6voice camera keyboard leds
         power haptics bluetooth ipa)

[ -d "$LINUX" ] || { echo "Kernelbaum fehlt, erst 20-fetch-sources.sh"; exit 1; }
[ -d "$NIXOS" ] || git clone -q --depth 1 \
  https://github.com/nyymi609/fxtec-pro1x-nixos.git "$NIXOS"

cd "$LINUX"
if [ ! -f .pro1x-patches-applied ]; then
  for p in "${PATCHES[@]}"; do
    echo ">> patch pro1x-$p"
    git apply -v "$NIXOS/device/kernel/patches/pro1x-$p.patch"
  done
  git add -A && git -c user.name=build -c user.email=build@local commit -qm \
    "Pro1X-Geraetepatches (NixOS-Port) auf v7.1"
  touch .pro1x-patches-applied
else
  echo "Patches schon angewendet"
fi

# Config des NixOS-Ports uebernehmen; olddefconfig raeumt GCC-spezifische
# Optionen weg, weil wir mit clang/lld bauen (kein aarch64-GCC auf dem Host).
cp "$NIXOS/device/kernel/config.aarch64" .config
MAKE=(make ARCH=arm64 LLVM=1 -j"$JOBS")
"${MAKE[@]}" olddefconfig
"${MAKE[@]}" Image.gz dtbs
"${MAKE[@]}" modules
mkdir -p "$OUT"
"${MAKE[@]}" modules_install INSTALL_MOD_PATH="$OUT/modules" >/dev/null

cp arch/arm64/boot/Image.gz "$OUT/Image.gz"
cp arch/arm64/boot/dts/qcom/sm6115-fxtec-pro1x.dtb "$OUT/sm6115-fxtec-pro1x.dtb"
echo "fertig:"
ls -la "$OUT/Image.gz" "$OUT/sm6115-fxtec-pro1x.dtb"
