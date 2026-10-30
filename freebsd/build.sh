#!/bin/bash
# Builds FreeBSD for the Pro1X on a Linux PC (x86_64, clang/lld, libarchive).
#
# The result is a kernel with its root filesystem built in, started in one
# of two ways (README.md):
#   out/kernel.bin        from the boot menu: the Linux initramfs kexecs it
#                         (copy to /host/freebsd/kernel.bin on the phone)
#   out/freebsd-boot.img  without Linux: `fastboot boot` hands it to the
#                         phone's bootloader, which starts it from RAM
#
#   WORK=/path/to/freebsd-build bash freebsd/build.sh
#   KERNFAST=1 ...   the kernel and modules again without a clean build
#   NOCLEAN=1 ...    the same after a change to PRO1X (config runs again)
#
# Steps: FreeBSD's sources (releng/15.1) with patches/, the cross toolchain
# (tools/build/make.py), a root filesystem from the release's /rescue
# (base.txz, SHA-256 checked) and mfsroot/, the PRO1X kernel.
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:?set WORK=<build directory>}
BRANCH=${BRANCH:-releng/15.1}
RELEASE=${RELEASE:-15.1-RELEASE}
BASE_SHA256=${BASE_SHA256:-5b7a46a0abfbe23a1d4454b5600e2efcfce16b705bc7e9c851c37470c035ef98}
# the Pro1X device tree of the Leste kernel build (scripts/30)
DTB=${DTB:-$WORK/../out/sm6115-fxtec-pro1x.dtb}
JOBS=${JOBS:-$(nproc)}
SRC=$WORK/src
OUT=$WORK/out
log() { echo ">> $*"; }
mkdir -p "$WORK" "$OUT" "$WORK/obj" "$WORK/dl"

# --- sources ---------------------------------------------------------------------
[ -d "$SRC" ] || git clone -q --depth 1 -b "$BRANCH" https://git.freebsd.org/src.git "$SRC"
for p in "$REPO"/freebsd/patches/*.patch; do
	mark=$SRC/.pro1x-applied-$(basename "$p" .patch)
	[ -f "$mark" ] && continue
	log "patch $(basename "$p")"
	git -C "$SRC" apply "$p"
	touch "$mark"
done

# --- toolchain ---------------------------------------------------------------------
# The build links host tools into its own PATH, GNU time among them.
mkdir -p "$WORK/hostbin"
if ! command -v time > /dev/null; then
	printf '#!/bin/sh\n# stand-in for GNU time, linked as a host tool\nexec "$@"\n' > "$WORK/hostbin/time"
	chmod 755 "$WORK/hostbin/time"
fi
export PATH=$WORK/hostbin:$PATH MAKEOBJDIRPREFIX=$WORK/obj
MAKE=(./tools/build/make.py --cross-bindir=/usr/bin --cross-compiler-type=clang
      --host-bindir=/usr/bin --host-compiler-type=clang
      TARGET=arm64 TARGET_ARCH=aarch64 -j"$JOBS" -DNO_WERROR WERROR=)
log "kernel toolchain (with makefs)"
(cd "$SRC" && "${MAKE[@]}" kernel-toolchain -DWITH_DISK_IMAGE_TOOLS_BOOTSTRAP)
MAKEFS=$(find "$WORK/obj" -path '*/tmp/legacy/*' -name makefs -type f | head -1)
[ -x "$MAKEFS" ] || { echo "makefs not built"; exit 1; }

# --- root filesystem -------------------------------------------------------------------
log "root filesystem from $RELEASE's /rescue"
B=$WORK/dl/base.txz
[ -f "$B" ] || curl -sSfL -o "$B" "https://download.freebsd.org/releases/arm64/$RELEASE/base.txz"
echo "$BASE_SHA256  $B" | sha256sum -c --quiet -
ROOT=$WORK/mfsroot
rm -rf "$ROOT"
mkdir -p "$ROOT"/{dev,tmp,bin,sbin}
bsdtar -xpf "$B" -C "$ROOT" ./rescue
ln -s /rescue/sh "$ROOT/bin/sh"
ln -s /rescue/init "$ROOT/sbin/init"
cp -a "$REPO/freebsd/mfsroot/." "$ROOT/"
"$MAKEFS" -t ffs -B little -o version=2 "$OUT/mfsroot.ufs" "$ROOT"

# --- kernel ----------------------------------------------------------------------------
log "kernel PRO1X"
sed -e "s|@MFS_IMAGE@|$OUT/mfsroot.ufs|" -e "s|@ENV_FILE@|$REPO/freebsd/pro1x.env|" \
	"$REPO/freebsd/PRO1X" > "$SRC/sys/arm64/conf/PRO1X"
# The DSP stack's drivers (dsp/, kernel modules for the root image): only
# these modules are built. The Wi-Fi: ath10k with its SNOC attachment, on
# LinuxKPI and net80211 (with their dependencies firmware and usb, and the
# ciphers wpa_supplicant's rc.d script loads)
cp -a "$REPO/freebsd/dsp/sys/." "$SRC/sys/"
WIFI_MODULES="firmware usb/usb linuxkpi linuxkpi_wlan wlan wlan_wep wlan_tkip"
WIFI_MODULES="$WIFI_MODULES wlan_ccmp wlan_gcmp athk_common ath10k_snoc"
# The Bluetooth/FM chip's UART (freebsd/fm/README.md): uart(4) with the
# GENI UART class, new files only; APR for the audio (qcom_apr, dsp/);
# the LPASS register windows for the FM sound's programmer (qcom_lpass)
cp -a "$REPO/freebsd/fm/sys/." "$SRC/sys/"
# The cameras (freebsd/camera/README.md): the CCI's buses, the camera
# clocks, pins and power domain, /dev/camss (qcom_camss), new files only
cp -a "$REPO/freebsd/camera/sys/." "$SRC/sys/"
MODULES=${MODULES:-qcom_dsp qrtr $WIFI_MODULES qcom_geni_uart qcom_apr qcom_lpass qcom_camss}
(cd "$SRC" && "${MAKE[@]}" KERNCONF=PRO1X MODULES_OVERRIDE="$MODULES" \
	-DWITH_KERNEL_BIN ${KERNFAST:+KERNFAST=1} ${NOCLEAN:+-DNO_KERNELCLEAN} \
	buildkernel)
K=$(find "$WORK/obj" -path '*/sys/PRO1X/kernel.bin' | head -1)
[ -f "$K" ] || { echo "kernel.bin not built"; exit 1; }
cp "$K" "$OUT/kernel.bin"
rm -rf "$OUT/modules"
mkdir -p "$OUT/modules"
for m in $MODULES; do
	ko=$(find "$WORK/obj" -path "*/sys/PRO1X/modules/*/$m.ko" | head -1)
	[ -f "$ko" ] || { echo "module $m not built"; exit 1; }
	cp "$ko" "$OUT/modules/"
done
# their dependencies by module name (linker.hints, made by the build's
# kldxref for the target)
KLDXREF=$(find "$WORK/obj" -path '*/tmp/legacy/bin/kldxref' -type f | head -1)
[ -x "$KLDXREF" ] && "$KLDXREF" "$OUT/modules"
# Their tools (dsp/tools), against the release's headers and libraries
BASEROOT=$WORK/base-sysroot
if [ ! -f "$BASEROOT/.done" ]; then
	rm -rf "$BASEROOT"
	mkdir -p "$BASEROOT"
	bsdtar -xf "$B" -C "$BASEROOT" ./usr/include ./usr/lib ./lib
	touch "$BASEROOT/.done"
fi
mkdir -p "$OUT/dsp-tools"
for t in "$REPO"/freebsd/dsp/tools/*/; do
	t=$(basename "$t")
	clang --target=aarch64-unknown-freebsd15.0 --sysroot="$BASEROOT" \
	    -fuse-ld=lld -O2 -Wall -I"$REPO/freebsd/dsp/sys/arm64/qualcomm/dsp" \
	    -o "$OUT/dsp-tools/$t" "$REPO/freebsd/dsp/tools/$t/$t.c"
done
# The FM radio's daemon, its client and its sound (freebsd/fm/tools)
for t in "$REPO"/freebsd/fm/tools/*/; do
	clang --target=aarch64-unknown-freebsd15.0 --sysroot="$BASEROOT" \
	    -fuse-ld=lld -O2 -Wall -I"$REPO/freebsd/dsp/sys/arm64/qualcomm/dsp" \
	    -o "$OUT/dsp-tools/$(basename "$t")" "$t"*.c
done
# The cameras' tool (freebsd/camera/tools/pro1x-cam)
clang --target=aarch64-unknown-freebsd15.0 --sysroot="$BASEROOT" \
    -fuse-ld=lld -O2 -Wall -I"$REPO/freebsd/camera/sys/arm64/qualcomm/camss" \
    -o "$OUT/dsp-tools/pro1x-cam" "$REPO"/freebsd/camera/tools/pro1x-cam/*.c -lz -lm
# and the test of the QRTR sockets (freebsd/dsp/test)
clang --target=aarch64-unknown-freebsd15.0 --sysroot="$BASEROOT" -fuse-ld=lld \
    -O2 -Wall -I"$REPO/freebsd/dsp/sys/arm64/qualcomm/dsp" \
    -o "$OUT/dsp-tools/qrtr_sock_test" "$REPO/freebsd/dsp/test/qrtr_sock_test.c"
# The Linux arm64 Image header: magic "ARMd" at 0x38. image_size (0x10)
# covers the kernel up to _end; FreeBSD then copies the device tree and
# builds its first page tables right behind _end. 8 MiB more, so that
# kexec places the device tree it hands over beyond that.
python3 - "$OUT/kernel.bin" <<'EOF'
import struct, sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    h = bytearray(f.read(64))
    assert h[0x38:0x3c] == b'ARMd', 'no arm64 Image header'
    size = struct.unpack_from('<Q', h, 0x10)[0]
    f.seek(0x10)
    f.write(struct.pack('<Q', size + (8 << 20)))
    print('image_size 0x%x -> 0x%x' % (size, size + (8 << 20)))
EOF

# --- boot image for fastboot boot -----------------------------------------------------------
# The bootloader's format, as for Leste (scripts/50-build-bootimg.sh): header
# v0, a gzip kernel with the device tree appended, the device tree with a
# board id. It appends its own arguments to the command line; FreeBSD then
# ignores the command line (pro1x.env carries the settings).
log "boot image for fastboot boot"
# shellcheck disable=SC1090
. "$REPO/device/boot_a-params.env"
cp "$DTB" "$OUT/pro1x-abl.dtb"
fdtget "$OUT/pro1x-abl.dtb" / qcom,board-id > /dev/null 2>&1 ||
	fdtput -t u "$OUT/pro1x-abl.dtb" / qcom,board-id 0 0
gzip -9 -n -c "$OUT/kernel.bin" > "$OUT/kernel.bin.gz"
cat "$OUT/kernel.bin.gz" "$OUT/pro1x-abl.dtb" > "$OUT/kernel.bin.gz-dtb"
mkbootimg --kernel "$OUT/kernel.bin.gz-dtb" --cmdline "FreeBSD:" \
	--base "$BOOT_BASE" --kernel_offset "$BOOT_KERNEL_OFFSET" \
	--ramdisk_offset "$BOOT_RAMDISK_OFFSET" --second_offset "$BOOT_SECOND_OFFSET" \
	--tags_offset "$BOOT_TAGS_OFFSET" --pagesize "$BOOT_PAGESIZE" \
	--header_version 0 --output "$OUT/freebsd-boot.img"
ls -la "$OUT/kernel.bin" "$OUT/freebsd-boot.img"
log "done"
