#!/bin/bash
# The PC test of oFono's IPA data path (patches/fixes/ofono.patch,
# docs/leste-mobile-data.md): the patched ofonod against a simulated modem
# on the PC's QRTR (fakemodem.py) and a dummy rmnet_ipa0 whose IPA sysfs
# files sysfs-shim.so supplies. Checks, per case, the DPM and WDA messages
# on the wire, oFono's fallbacks and the rmnet links it makes (data format
# read back from the kernel with tools/pro1x-rmnet-flags). Not covered:
# what a real modem answers, the data contexts, icd2.
#
# Build host, needs sudo (QRTR, rmnet and dummy modules, a dummy network
# device for the run, mount namespaces). Two ways to get an ofonod:
#
#   bash tests/ofono-ipa/run.sh [CASE...]
#       the release's arm64 packages (RELEASE, default below; or DEBS=dir
#       with ofono_*.deb) installed in leste-chroot.sh's chroot of Leste's
#       image and run there under qemu. BASE_IMG, CHROOT, MIN_FREE_GB as
#       leste-chroot.sh; the chroot is unmounted at the end, DELETE=1 also
#       removes it.
#   OFONOD=/path/to/ofonod bash tests/ofono-ipa/run.sh [CASE...]
#       an ofonod built on the PC (oFono 2.18 with the patch, configure
#       --disable-bluetooth --disable-dundee is enough).
#
# CASE: v5 qmap refuse missing nodpm nowda silent stale override (default
# all). KEEP=1 keeps the result directory (/tmp/ofono-ipa-test.*).
#
# Safety: the cases run in the PC's initial network namespace (QRTR has no
# other), so the script refuses to start while the PC has a rmnet_ipa0 or
# any rmnet link (oFono deletes every rmnet link when it starts); udevng
# also enumerates the PC's devices: no USB modem should be plugged in.
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$T/../.." && pwd)}
WORK=${WORK:-$(cd "$REPO/.." && pwd)}
export CHROOT=${CHROOT:-$WORK/build/leste-chroot}
RELEASE=${RELEASE:-leste-mobile-data-2026.10.30}
GH_REPO=${GH_REPO:-smatkovi/pro1x-maemo-leste}
OFONOD=${OFONOD:-}
DEBS=${DEBS:-}
KEEP=${KEEP:-0}
DELETE=${DELETE:-0}
ALL="v5 qmap refuse missing nodpm nowda silent stale override"
CASES=${*:-$ALL}
FLAGS_TOOL=$REPO/tools/pro1x-rmnet-flags
C=(bash "$REPO/scripts/leste-chroot.sh")
log() { echo ">> $*"; }

for c in $CASES; do
  case " $ALL " in *" $c "*) ;; *) echo "unknown case $c ($ALL)"; exit 2 ;; esac
done
if ip link show rmnet_ipa0 > /dev/null 2>&1 ||
    [ "$(python3 -I "$FLAGS_TOOL" 2> /dev/null | grep -c ' parent ')" != 0 ]; then
  echo "the PC has rmnet_ipa0 or rmnet links: not running (oFono would delete them)"
  exit 1
fi

WD=$(mktemp -d /tmp/ofono-ipa-test.XXXXXX)
chmod 755 "$WD"
loaded=()
MOUNTED=
finish() {
  set +e
  if [ -n "$MOUNTED" ]; then
    "${C[@]}" umount
    [ "$DELETE" != 1 ] || "${C[@]}" delete
  fi
  [ ${#loaded[@]} = 0 ] || sudo -n modprobe -r "${loaded[@]}"
  if [ "$KEEP" = 1 ]; then echo ">> results in $WD"; else sudo -n rm -rf "$WD"; fi
}
trap finish EXIT
for m in qrtr rmnet dummy; do
  if ! lsmod | grep -q "^$m "; then
    sudo -n modprobe "$m"
    loaded+=("$m")
  fi
done

if [ -n "$OFONOD" ]; then
  ROOT=
  gcc -shared -fPIC -O1 -Wall -o "$WD/sysfs-shim.so" "$T/sysfs-shim.c" -ldl
  SHIM=$WD/sysfs-shim.so
  log "ofonod: $OFONOD (on the PC)"
else
  mkdir -p "$WD/debs"
  if [ -n "$DEBS" ]; then
    cp "$DEBS"/ofono_*.deb "$WD/debs/"
  else
    log "release $RELEASE"
    gh release download "$RELEASE" -R "$GH_REPO" -p 'ofono_*' -p SHA256SUMS -D "$WD/debs"
    (cd "$WD/debs" && sha256sum -c --ignore-missing --quiet SHA256SUMS && rm SHA256SUMS)
  fi
  "${C[@]}" mount
  MOUNTED=1
  ROOT=$CHROOT/root
  sudo -n rm -rf "$ROOT/tmp/ofono-ipa"
  sudo -n mkdir -p "$ROOT/tmp/ofono-ipa"
  sudo -n cp "$WD"/debs/ofono_*.deb "$T/sysfs-shim.c" "$ROOT/tmp/ofono-ipa/"
  "${C[@]}" run sh -c 'apt-get update -q > /tmp/ofono-ipa/apt.log 2>&1;
    apt-get install -y -q --no-install-recommends --allow-downgrades \
      gcc libc6-dev /tmp/ofono-ipa/ofono_*.deb >> /tmp/ofono-ipa/apt.log 2>&1 ||
      { tail -20 /tmp/ofono-ipa/apt.log; exit 1; }
    gcc -shared -fPIC -O1 -Wall -o /tmp/ofono-ipa/sysfs-shim.so \
      /tmp/ofono-ipa/sysfs-shim.c -ldl'
  SHIM=/tmp/ofono-ipa/sysfs-shim.so
  OFONOD=/usr/sbin/ofonod
  log "ofonod: $("${C[@]}" run dpkg-query -W -f='${Package} ${Version}' ofono) (Leste chroot, qemu)"
fi

# --- the cases ---------------------------------------------------------------
fail=0
ok() { echo "   ok   $*"; }
bad() { echo "   FAIL $*"; fail=$((fail + 1)); }
has() { if grep -q -F -- "$2" "$W/$1"; then ok "$3"; else bad "$3 ($1: $2)"; fi; }
# msg MESSAGE TLV TEXT: a request MESSAGE (fake.log) that carries TLV
msg() {
  if grep -F -- "$1" "$W/fake.log" | grep -q -F -- "$2"; then ok "$3"; else bad "$3 (fake.log: $1 with $2)"; fi
}
hasnt() { if grep -q -F -- "$2" "$W/$1"; then bad "$3 ($1: $2)"; else ok "$3"; fi; }
powered() {
  if grep -A1 '"Powered"' "$W/props.txt" | grep -q 'boolean true'; then
    ok "modem powered"
  else
    bad "modem not powered (props.txt)"
  fi
}
links() {   # links FLAGS NAMES
  local n
  n=$(grep -c -E "^rmnet[0-3] parent rmnet_ipa0 mux_id [1-4] flags $1 $2\$" "$W/rmnet.txt" || true)
  if [ "$n" = 4 ]; then ok "rmnet0..3 on rmnet_ipa0, flags $1"; else bad "rmnet links ($n of 4 with flags $1: rmnet.txt)"; fi
}
nolinks() { has rmnet.txt "no rmnet links" "no rmnet links"; }
sets() {    # the WDA Set Data Format protocols in order
  local got
  got=$(grep 'WDA msg 0x0020' "$W/fake.log" | grep -o "'0x12': '[0-9a-f]*'" | cut -c10-11 | tr '\n' ' ' || true)
  if [ "$got" = "$1" ]; then ok "WDA set: $1"; else bad "WDA set: $got, wanted $1"; fi
}
proto() { has ofonod.log "WDA data format: raw IP, aggregation protocol $1" "WDA protocol $1"; }
dpm() { has fake.log "DPM msg 0x0020 txn" "DPM open port sent" &&
  has fake.log "'0x11': '0104000000010000000100000009000000'" "DPM TLV: embedded, interface 1, RX 1, TX 9"; }
V4="INGRESS_DEAGGREGATION INGRESS_MAP_CKSUMV4 EGRESS_MAP_CKSUMV4"

check() {
  case $1 in
  v5) has ofonod.log "Mainline IPA driver: no premux interfaces" "udevng: mainline IPA"
    dpm; sets "09 "; proto 9; powered; links 0xd "$V4" ;;
  qmap) sets "09 08 05 "; proto 5; powered; links 0xd "$V4" ;;
  refuse) sets "09 08 05 "; proto 5; powered; links 0xd "$V4" ;;
  missing) msg "WDA msg 0x0021" "'0x10': '0400000001000000'" "Get again with the endpoint info"
    msg "WDA msg 0x0020" "'0x17': '0400000001000000'" "Set with the endpoint info"
    proto 9; powered; links 0xd "$V4" ;;
  nodpm) has ofonod.log "modem has no DPM service" "no DPM service noticed"
    hasnt fake.log "DPM msg" "no DPM message"; proto 9; powered; links 0xd "$V4" ;;
  nowda) sets "09 08 05 "
    has ofonod.log "no mobile data: the modem took none of QMAPv5, QMAPv4 and QMAP (WDA)" "gives up on WDA"
    powered; nolinks ;;
  silent) has ofonod.log "no mobile data: no answer from the modem" "20 s timeout"
    powered; nolinks ;;
  stale) for n in 0 1 2 3; do
      has ofonod.log "Removing existing rmnet link: rmnet_data$n(" "stale rmnet_data$n deleted"
    done
    hasnt links.txt "rmnet_data" "no rmnet_data left"; proto 9; powered; links 0xd "$V4" ;;
  override) has ofonod.log "OFONO_IPA_DPM=0: no DPM open port" "OFONO_IPA_DPM=0"
    hasnt fake.log "DPM msg" "no DPM message"
    has ofonod.log "OFONO_IPA_WDA: only qmap" "OFONO_IPA_WDA=qmap"; sets "05 "; proto 5
    has ofonod.log "OFONO_IPA_RMNET_FLAGS: 0x1 instead of 0xd" "OFONO_IPA_RMNET_FLAGS=0x1"
    powered; links 0x1 "INGRESS_DEAGGREGATION" ;;
  esac
}

for c in $CASES; do
  W=$WD/$c
  mkdir -p "$W"
  chmod 777 "$W"
  sc=$c stale= wda= dpmv= flags=
  case $c in
  stale) sc=v5 stale=1 ;;
  override) sc=v5 wda=qmap dpmv=0 flags=0x1 ;;
  esac
  log "case $c"
  sudo -n env W="$W" T="$T" SC="$sc" OFONOD="$OFONOD" ROOT="$ROOT" SHIM="$SHIM" \
    FLAGS_TOOL="$FLAGS_TOOL" STALE="$stale" OFONO_IPA_WDA="$wda" OFONO_IPA_DPM="$dpmv" \
    OFONO_IPA_RMNET_FLAGS="$flags" unshare -m bash "$T/run-one.sh" || bad "run-one.sh failed"
  check "$c"
done

if [ "$fail" = 0 ]; then
  echo ">> all passed ($CASES)"
else
  echo ">> $fail checks failed"
  KEEP=1
  exit 1
fi
