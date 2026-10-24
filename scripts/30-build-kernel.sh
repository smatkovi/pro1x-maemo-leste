#!/bin/bash
# Builds the mainline kernel 7.1 with the Pro1X device patches (build host, x86_64).
#
# The qx1050-7.1 branch of sm6115-mainline is identical to upstream v7.1;
# the complete device support lives in the patches of the NixOS port, which
# are applied here in exactly that port's order (later patches edit the same
# device tree).
set -euo pipefail
WORK=${WORK:?set WORK=<build-dir>}
LINUX=$WORK/build/linux
NIXOS=$WORK/build/nixos-pro1x
OUT=$WORK/out
JOBS=${JOBS:-$(nproc)}

PATCHES=(touch egpio dsi panelgap dpugap audio q6voice camera keyboard leds
         power haptics bluetooth ipa)

[ -d "$LINUX" ] || { echo "kernel tree missing, run 20-fetch-sources.sh first"; exit 1; }
[ -d "$NIXOS" ] || git clone -q --depth 1 \
  https://github.com/nyymi609/fxtec-pro1x-nixos.git "$NIXOS"

cd "$LINUX"
if [ ! -f .pro1x-patches-applied ]; then
  for p in "${PATCHES[@]}"; do
    echo ">> patch pro1x-$p"
    git apply -v "$NIXOS/device/kernel/patches/pro1x-$p.patch"
  done
  git add -A && git -c user.name=build -c user.email=build@local commit -qm \
    "Pro1X device patches (NixOS port) on top of v7.1"
  touch .pro1x-patches-applied
else
  echo "patches already applied"
fi

# Take over the NixOS port's config; olddefconfig drops GCC-specific options,
# because we build with clang/lld (no aarch64 GCC on the host).
cp "$NIXOS/device/kernel/config.aarch64" .config
MAKE=(make ARCH=arm64 LLVM=1 -j"$JOBS")
"${MAKE[@]}" olddefconfig
"${MAKE[@]}" Image.gz dtbs
"${MAKE[@]}" modules
mkdir -p "$OUT"
"${MAKE[@]}" modules_install INSTALL_MOD_PATH="$OUT/modules" >/dev/null

cp arch/arm64/boot/Image.gz "$OUT/Image.gz"
cp arch/arm64/boot/dts/qcom/sm6115-fxtec-pro1x.dtb "$OUT/sm6115-fxtec-pro1x.dtb"
echo "done:"
ls -la "$OUT/Image.gz" "$OUT/sm6115-fxtec-pro1x.dtb"
