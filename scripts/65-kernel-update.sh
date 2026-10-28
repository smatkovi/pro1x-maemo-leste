#!/bin/bash
# A new kernel for every Linux system on the phone (Leste, the MeeGo copy,
# Kali, LuneOS, Arch: they all boot boot_b's kernel), from the build host.
#
# The kernel's modules go to one place on userdata, /host/pro1x-modules/
# <version>, which the initramfs (init-bottom/pro1x-shared-modules) binds
# into whichever system it starts. So no image needs its own copy, an older
# Arch snapshot gets the modules of the kernel that boots it, and earlier
# versions stay for a boot image flashed back. That needs a version of its
# own per kernel: build with KLOCAL (30-build-kernel.sh), e.g.
#   KLOCAL=-pro1x.2 WORK=... bash scripts/30-build-kernel.sh
#
#   PHONE="ssh ... root@172.16.42.1" WORK=... bash scripts/65-kernel-update.sh prepare
#       modules to the phone (the running kernel's too, if not there yet),
#       the initramfs scripts of this repository into Leste, the initramfs
#       for the new kernel made in Leste, the boot image here (50), on the
#       phone as /host/boot_b-<version>.img. Writes no partition.
#   ... 65-kernel-update.sh refresh
#       prepare for the kernel that runs already (its modules are on the
#       phone): the initramfs and the boot image made again, after changes
#       to the boot menu or the initramfs scripts only. Then flash with
#       TRIES=0 (the kernel is known to start).
#   ... 65-kernel-update.sh flash
#       saves boot_b as /host/boot_b-before-<version>.img, then writes the
#       prepared image into boot_b (60-flash-slot-b.sh) and puts slot b on
#       trial: TRIES starts (3), which pro1x-boot-trial confirms in the
#       system or the bootloader counts as failed; after the last it
#       starts slot a, Sailfish OS (TRIES=0: no trial). Does not reboot.
#       Only with the phone's owner there: a kernel that does not boot
#       leaves the phone in fastboot or hanging (back: fastboot, or Sailfish
#       in slot a, then boot_b-before-<version>.img into boot_b).
#   ... 65-kernel-update.sh status
#       the versions on the phone, the running kernel, the saved images.
#
# Btrfs snapshots do not cover any of this: boot_b (kernel, initramfs, boot
# menu) is a partition of its own, its way back is the saved image.
set -euo pipefail
WORK=${WORK:?set WORK=<build dir>}
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
PHONE=${PHONE:?set PHONE=<ssh command for root on the phone in Leste>}
# OUT: 30-build-kernel.sh's (and 50-build-bootimg.sh's) output directory
OUT=${OUT:-$WORK/out}
STORE=/host/pro1x-modules
log() { echo ">> $*"; }
ph() { $PHONE "$@"; }

KVER=$(cat "$OUT/kernelrelease" 2>/dev/null || true)
[ -n "$KVER" ] || { echo "no $OUT/kernelrelease: build with 30-build-kernel.sh first"; exit 1; }
IMG=/host/boot_b-$KVER.img

case "${1:-}" in
status)
	ph "echo running: \$(uname -r); echo modules on userdata:; ls $STORE 2>/dev/null || echo '  (none)'; echo boot images:; ls -la /host/boot_b-*.img"
	exit 0 ;;
prepare|refresh|flash) ;;
*)	sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 1 ;;
esac

running=$(ph uname -r)
[ "$1" != prepare ] || [ "$KVER" != "$running" ] || {
	echo "the phone runs $KVER already: give the new kernel a version of its own (KLOCAL), or refresh"
	exit 1
}
[ "$1" != refresh ] || [ "$KVER" = "$running" ] ||
    { echo "refresh is for the running kernel ($running), not $KVER"; exit 1; }

if [ "$1" = prepare ] || [ "$1" = refresh ]; then
	for f in "$OUT/Image.gz" "$OUT/sm6115-fxtec-pro1x.dtb" "$OUT/config-$KVER" \
	    "$OUT/modules/lib/modules/$KVER/modules.dep"; do
		[ -f "$f" ] || { echo "missing: $f"; exit 1; }
	done

	if [ "$1" = prepare ]; then
	log "modules $KVER to $STORE"
	# (a version that is there already is replaced: it is not the running one)
	tar -C "$OUT/modules/lib/modules" --owner=0 --group=0 -cf - "$KVER" |
	    ph "set -e; mkdir -p $STORE; rm -rf $STORE/.new-$KVER; mkdir $STORE/.new-$KVER
		tar -C $STORE/.new-$KVER -xf -
		rm -rf $STORE/$KVER; mv $STORE/.new-$KVER/$KVER $STORE/$KVER; rmdir $STORE/.new-$KVER
		rm -f $STORE/$KVER/build $STORE/$KVER/source"
	log "modules of the running kernel ($running) to $STORE, if not there yet"
	ph "[ -d $STORE/$running ] || cp -a /lib/modules/$running $STORE/$running"
	fi

	log "pro1x-boot and the trial's parts into the systems (66-boot-trial-install.sh)"
	tar -C "$REPO" --owner=0 --group=0 -cf - tools/pro1x-boot tools/pro1x-boot-trial \
	    scripts/66-boot-trial-install.sh |
	    ph "set -e; t=\$(mktemp -d); tar -C \$t -xf -; REPO=\$t bash \$t/scripts/66-boot-trial-install.sh; rm -r \$t"

	log "initramfs scripts of this repository into Leste"
	tar -C "$REPO/initramfs" --owner=0 --group=0 -cf - hooks local-top init-bottom |
	    ph "set -e; t=\$(mktemp -d); tar -C \$t -xf -
		install -m 755 \$t/hooks/* /etc/initramfs-tools/hooks/
		install -m 755 \$t/local-top/* /etc/initramfs-tools/scripts/local-top/
		install -m 755 \$t/init-bottom/* /etc/initramfs-tools/scripts/init-bottom/
		rm -r \$t"
	ph "cat > /boot/config-$KVER" < "$OUT/config-$KVER"

	log "initramfs for $KVER (made in Leste, with $STORE/$KVER as its modules)"
	ph "set -e; d=/lib/modules/$KVER; made=
		if [ ! -e \$d ]; then mkdir \$d; made=1; fi
		mount --bind $STORE/$KVER \$d
		trap 'umount \$d; [ -z \"\$made\" ] || rmdir \$d' EXIT
		mkdir -p /host/kernel-update
		mkinitramfs -o /host/kernel-update/initrd.img-$KVER $KVER"
	ph "cat /host/kernel-update/initrd.img-$KVER" > "$OUT/initrd.img-$KVER"

	log "boot image (50-build-bootimg.sh)"
	KVER=$KVER WORK=$WORK OUT=$OUT bash "$REPO/scripts/50-build-bootimg.sh" > "$OUT/bootimg-$KVER.log" 2>&1 ||
	    { tail -20 "$OUT/bootimg-$KVER.log"; exit 1; }
	cp "$OUT/boot-leste.img" "$OUT/boot_b-$KVER.img"
	log "to the phone: $IMG"
	ph "cat > $IMG.part && mv $IMG.part $IMG" < "$OUT/boot_b-$KVER.img"
	here=$(sha256sum < "$OUT/boot_b-$KVER.img" | cut -d' ' -f1)
	there=$(ph "sha256sum < $IMG" | cut -d' ' -f1)
	[ "$here" = "$there" ] || { echo "the image on the phone differs from this one"; exit 1; }
	log "prepared: $IMG ($here); write it with: $0 flash"
	exit 0
fi

# flash
ph "[ -f $IMG ]" || { echo "$IMG is not there: run prepare first"; exit 1; }
ph "[ -f $STORE/$KVER/modules.dep ]" || { echo "$STORE/$KVER is missing: run prepare first"; exit 1; }
log "boot_b now to /host/boot_b-before-$KVER.img"
ph "set -e; dd if=/dev/disk/by-partlabel/boot_b of=/host/boot_b-before-$KVER.img bs=1M status=none; sync"
# and here, for fastboot if the new kernel does not come up
ph "cat /host/boot_b-before-$KVER.img" > "$OUT/boot_b-before-$KVER.img"
[ "$(sha256sum < "$OUT/boot_b-before-$KVER.img" | cut -d' ' -f1)" = \
    "$(ph "sha256sum < /host/boot_b-before-$KVER.img" | cut -d' ' -f1)" ] ||
    { echo "the saved boot_b did not arrive here intact: not flashing"; exit 1; }
log "saved: $OUT/boot_b-before-$KVER.img (fastboot flash boot_b ...)"
log "writing $IMG into boot_b"
ph "TARGET=b IMG=$IMG VBMETA= sh -s flash" < "$REPO/scripts/60-flash-slot-b.sh"
# The trial (pro1x-boot trial, docs/kernel-update.md): $TRIES starts of
# slot b, each confirmed by pro1x-boot-trial or counted as failed by the
# bootloader, which then starts slot a (Sailfish OS). TRIES=0: no trial.
TRIES=${TRIES:-3}
if [ "$TRIES" != 0 ]; then
	ph "/usr/local/sbin/pro1x-boot trial $KVER $TRIES"
else
	log "no trial (TRIES=0): slot b stays successful"
fi
log "done: $KVER boots after the next reboot (not done here); back: TARGET=b IMG=/host/boot_b-before-$KVER.img VBMETA= sh 60-flash-slot-b.sh flash"
