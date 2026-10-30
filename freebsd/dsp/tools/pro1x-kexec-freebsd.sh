#!/bin/sh
# pro1x-kexec-freebsd.sh [-n] [CMDLINE]: FreeBSD started from a running
# Maemo Leste with kexec, the modem (and the ADSP) left running for
# FreeBSD to take over (freebsd/dsp/README.md, "The handover"): the
# second way to a running DSP, next to FreeBSD starting them itself after
# the boot menu's kexec. A PROPOSAL: not yet run on the phone. Not
# installed into any image; copy it to Leste and run it there as root.
#
#   CMDLINE   FreeBSD's kernel environment, default: the adopt run's
#             (/host/freebsd/cmdline.dsp-adopt from build-dsp.sh, else
#             "pro1x.test=dsp pro1x.dsp=adopt pro1x.pool=off
#             hint.qcom_wdt.0.lifetime=300")
#   -n        only load the kernel and unload it again (a check)
#
# What it does, after KolibriOS's tools/pro1x-kexec.sh -m and the boot
# menu's FreeBSD start (initramfs/local-top/pro1x-loop-root):
#
# 1. ofono stops: its QMI clients go away and QRTR tells the modem.
#    rmtfs, tqftpserv and pd-mapper stay: Leste's rmtfs -s stops the
#    modem on SIGTERM. IPCRTR stays open (FreeBSD finds its ids in the
#    GLINK FIFOs and adopts it): unbinding qcom_smd_qrtr from it hung
#    Linux 7.1 before device/kernel/patches/0022, and the WLAN firmware
#    crashed with QRTR gone under ath10k (KolibriOS, docs/pro1x-modem.md).
# 2. kexec_file loads /host/freebsd/kernel.bin with the device tree this
#    kernel booted with; FreeBSD reads bootargs from "FreeBSD:" on.
# 3. Filesystems synced and remounted read-only (sysrq s, u).
# 4. UFS rebound with an identity SMMU mapping, as the boot menu does it
#    for FreeBSD (no SMMU driver there): this is the controller of Leste's
#    own root, which is read-only and gone from here on; what follows runs
#    from what the shell already holds (the function below) and the page
#    cache (kexec, busybox: run once before, so their pages are there).
# 5. The APSS watchdog armed for its longest time (about 32 s), as the
#    boot menu does: FreeBSD feeds it; a hang resets the phone.
# 6. kexec -e. Linux's shutdown hooks run on the way (ath10k switches the
#    WLAN firmware off over QRTR); qcom_q6v5_pas has none: the DSPs run on.
#
# After the next boot, Leste's fbsdtest.sh shows FreeBSD's RAM console
# (the "=== dsp-adopt: ..." summary).
set -eu

check=
[ "${1:-}" = -n ] && { check=1; shift; }
host=/host/freebsd
kernel=$host/kernel.bin
if [ $# -gt 0 ]; then
	args=$1
elif [ -f $host/cmdline.dsp-adopt ]; then
	args=$(cat $host/cmdline.dsp-adopt)
else
	args="pro1x.test=dsp pro1x.dsp=adopt pro1x.pool=off hint.qcom_wdt.0.lifetime=300"
fi
# the time: the RTC only counts, Leste keeps the offset (as the boot menu)
off=$(cat /host/pro1x-rtc-offset 2>/dev/null || true)
case "$off" in
''|*[!0-9-]*) ;;
*) args="$args hint.qcom_rtc.0.offset=$off" ;;
esac
args="FreeBSD:$args"

[ "$(id -u)" = 0 ] || { echo "pro1x-kexec-freebsd: as root" >&2; exit 1; }
command -v kexec > /dev/null || { echo "pro1x-kexec-freebsd: no kexec" >&2; exit 1; }
[ -f "$kernel" ] || { echo "pro1x-kexec-freebsd: no $kernel" >&2; exit 1; }
busybox devmem 0x0f017008 32 > /dev/null ||
    { echo "pro1x-kexec-freebsd: no busybox devmem (the watchdog)" >&2; exit 1; }
ufs=/sys/bus/platform/devices/4804000.ufshc
grp=$(readlink "$ufs/iommu_group" 2>/dev/null || true)
[ -n "$grp" ] || { echo "pro1x-kexec-freebsd: no IOMMU group for UFS" >&2; exit 1; }
# what FreeBSD takes over: the modem running (its remoteproc)
for rp in /sys/class/remoteproc/remoteproc*; do
	echo "pro1x-kexec-freebsd: $(cat $rp/name 2>/dev/null): $(cat $rp/state 2>/dev/null)"
done

kexec -s -l "$kernel" --command-line="$args"
trap 'kexec -u; rc-service ofono start > /dev/null 2>&1 || true' EXIT
if [ -n "$check" ]; then
	echo "pro1x-kexec-freebsd: $kernel loaded with \"$args\", unloaded again"
	exit 0
fi

# (1) the modem's clients go; its servers stay
rc-service ofono stop > /dev/null 2>&1 || true
sleep 1

# (3)-(6) from here on nothing is read from the root: one function, its
# programs' pages warm
kexec --version > /dev/null 2>&1 || true
busybox true
jump() {
	sync
	echo s > /proc/sysrq-trigger
	echo u > /proc/sysrq-trigger
	sleep 3
	echo 4804000.ufshc > $ufs/driver/unbind
	echo identity > "/sys/kernel/iommu_groups/${grp##*/}/type" || true
	echo 4804000.ufshc > /sys/bus/platform/drivers/ufshcd-qcom/bind
	# UFS back (its disks in /sys), else no jump: FreeBSD would set it up
	# behind the SMMU and fault there, into the bootloader's crash dump
	# mode (the boot menu's reason for the same check); a reboot instead
	i=0
	while [ ! -e /sys/block/sda ] && [ $i -lt 20 ]; do
		busybox sleep 0.5
		i=$((i + 1))
	done
	if [ ! -e /sys/block/sda ]; then
		echo "pro1x-kexec-freebsd: UFS did not come back: rebooting" > /dev/kmsg
		echo b > /proc/sysrq-trigger
	fi
	busybox devmem 0x0f017008 32 0
	busybox devmem 0x0f017004 32 1
	busybox devmem 0x0f017010 32 0xfffff
	busybox devmem 0x0f017014 32 0xfffff
	busybox devmem 0x0f017008 32 1
	kexec -e
	# not reached; the watchdog resets the phone into the boot menu
}
trap - EXIT
jump
