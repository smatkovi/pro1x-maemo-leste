#!/bin/bash
# PRO1XQRNG (freebsd/rng/build.sh qemu) in QEMU's virt machine, one CPU,
# the console in $RNGOUT/qemu-NAME.log; prints the random lines and checks
# them:
#
#   WORK=... bash freebsd/rng/test/qemu/run.sh [seed] [noseed] [prng] [clock]
#
#   seed     QEMU's own tree: its /chosen/rng-seed (32 bytes) and kaslr-seed
#            (8) taken into pool 0 and wiped in the tree
#   noseed   dtb-randomness=off: no seed in the tree, nothing said
#   prng     a tree with a qcom,prng-ee node and a qcom,gcc-sm6115 node on
#            RAM (QEMU's loader: STATUS set, one word): qcom_rnd votes the
#            clock, reads the same word twice and does not register
#   clock    the same with the clock's CLK_OFF stuck: refused without
#            reading the PRNG
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:?set WORK=<build directory>}
OUT=${RNGOUT:-$WORK/rng-out}
K=$OUT/kernel-qemu.bin
[ -f "$K" ] || { echo "no $K: freebsd/rng/build.sh qemu"; exit 1; }
PRNG=0x50000000
fails=0

qemu() {	# qemu NAME MACHINE [QEMU ARGUMENTS...]
	local name=$1 machine=$2
	shift 2
	timeout ${TIMEOUT:-300} qemu-system-aarch64 -M "$machine" -cpu cortex-a53 \
	    -smp 1 -m 2G -nographic -no-reboot -kernel "$K" "$@" < /dev/null \
	    > "$OUT/qemu-$name.log" 2>&1 || true
	tr -d '\r' < "$OUT/qemu-$name.log" | grep -E "^random:|^qcom_rnd|^kern.random|^dev.qcom_rnd|^=== rng-qemu|panic"
}

expect() {	# expect NAME PATTERN... (each a line of the log, in any order)
	local name=$1 p
	shift
	for p in "$@"; do
		if ! tr -d '\r' < "$OUT/qemu-$name.log" | grep -qE "$p"; then
			echo "FAIL $name: no line like: $p"
			fails=$((fails + 1))
			return
		fi
	done
	echo "ok $name"
}

refuse() {	# refuse NAME PATTERN: no such line
	if tr -d '\r' < "$OUT/qemu-$1.log" | grep -qE "$2"; then
		echo "FAIL $1: a line like: $2"
		fails=$((fails + 1))
	fi
}

# a tree with the PRNG and its GCC on RAM; the RAM's words from a file
prng_tree() {	# prng_tree NAME CBCR
	local name=$1 cbcr=$2 d=$OUT/$1
	qemu-system-aarch64 -M "virt,gic-version=3,dumpdtb=$d.virt.dtb" -cpu cortex-a53 \
	    -smp 1 -m 2G -display none > /dev/null 2>&1
	dtc -q -I dtb -O dts "$d.virt.dtb" | python3 -c '
import sys
dts = sys.stdin.read()
nodes = """
	reserved-memory {
		#address-cells = <0x02>;
		#size-cells = <0x02>;
		ranges;
		prng@50000000 {
			reg = <0x00 0x50000000 0x00 0x200000>;
			no-map;
		};
	};

	gcc: clock-controller@50100000 {
		compatible = "qcom,gcc-sm6115";
		reg = <0x00 0x50100000 0x00 0x1f0000>;
		#clock-cells = <1>;
	};

	rng@50000000 {
		compatible = "qcom,prng-ee";
		reg = <0x00 0x50000000 0x00 0x1000>;
		clocks = <&gcc 37>;
		clock-names = "core";
	};
"""
i = dts.rfind("};")
print(dts[:i] + nodes + dts[i:])' | dtc -q -I dts -O dtb -o "$d.dtb" -
	python3 - "$d.bin" "$cbcr" <<'PY'
import struct, sys
b = bytearray(0x200000)
struct.pack_into('<I', b, 0, 0x12345678)        # DATA_OUT: one word, always
struct.pack_into('<I', b, 4, 1)                 # STATUS: a word is there
struct.pack_into('<I', b, 0x100000 + 0x21004, int(sys.argv[2], 0))   # the CBCR
open(sys.argv[1], 'wb').write(b)
PY
}

for t in ${*:-seed noseed prng clock}; do
	case $t in
	seed)
		qemu seed virt,gic-version=3
		expect seed "^random: 32 bytes of the device tree's rng-seed, wiped in the tree" \
		    "^random: 8 bytes of the device tree's kaslr-seed, wiped in the tree" \
		    "^kern.random.harvest.fdt_seed: 40" "^=== rng-qemu: /dev/random reads" ;;
	noseed)
		qemu noseed virt,gic-version=3,dtb-randomness=off
		expect noseed "^kern.random.harvest.fdt_seed: 0" "^=== rng-qemu: /dev/random reads"
		refuse noseed "^random: .* bytes of the device tree" ;;
	prng)
		prng_tree prng 0
		qemu prng virt,gic-version=3 -dtb "$OUT/prng.dtb" \
		    -device "loader,file=$OUT/prng.bin,addr=$PRNG,force-raw=on"
		expect prng "^qcom_rnd0: .*gives no words \(zeros 0, timeouts 0, repeats 1, clock stuck 0\): not used" \
		    "^=== rng-qemu: /dev/random reads"
		refuse prng "Qualcomm Entropy Adapter" ;;
	clock)
		prng_tree clock 0x80000000
		qemu clock virt,gic-version=3 -dtb "$OUT/clock.dtb" \
		    -device "loader,file=$OUT/clock.bin,addr=$PRNG,force-raw=on"
		expect clock "^qcom_rnd0: .*gives no words \(zeros 0, timeouts 0, repeats 0, clock stuck 1\): not used" ;;
	*)	echo "unknown test $t"; exit 1 ;;
	esac
done
[ $fails = 0 ] && echo "rng-qemu: all passed" || { echo "rng-qemu: $fails failed"; exit 1; }
