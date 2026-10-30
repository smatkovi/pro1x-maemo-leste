#!/bin/bash
# PRO1XQ in QEMU's virt machine (8 Cortex-A53, GICv3, PSCI over hvc), the
# SM6115's idle topology in the device tree (mkdtb.py), the root
# filesystem's rc (rc) doing its work and powering off:
#
#   WORK=... bash freebsd/pm/test/qemu/run.sh NAME "kenv settings"
#
# e.g. run.sh off "hw.psci_idle.enable=0"
#      run.sh core "hw.psci_idle.enable=1 hw.psci_idle.simulate=1 hw.psci_idle.depth=1"
#      run.sh cluster "hw.psci_idle.enable=1 hw.psci_idle.simulate=1 hw.psci_idle.pc_cluster=1"
# The console goes to $WORK/pm-out/qemu-NAME.log. SMP=N CPUs (8).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:?set WORK=<build directory>}
NAME=${1:?name}
ENV=${2:-}
SMP=${SMP:-8}
OUT=$WORK/pm-out
python3 -I "$HERE/mkdtb.py" --smp "$SMP" "$OUT/pm-$SMP.dtb" > /dev/null
timeout ${TIMEOUT:-600} qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 \
    -smp "$SMP" -m 2G -nographic -no-reboot -kernel "$OUT/kernel-qemu.bin" \
    -dtb "$OUT/pm-$SMP.dtb" -append "FreeBSD:$ENV" < /dev/null \
    > "$OUT/qemu-$NAME.log" 2>&1 || true
grep -E "^psci_idle|^=== |worker|^cpu[0-9]|broadcast|panic|!!|hw.ncpu|kern.smp|^kern.cp_times" \
    "$OUT/qemu-$NAME.log" | tr -d '\r'
