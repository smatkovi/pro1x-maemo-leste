#!/bin/bash
# Qualcomm's Android bootloader (ABL, edk2's LinuxLoader) for the F(x)tec
# Pro1X with the patches in abl/patches, built in a Debian 11 container
# (this edk2's build tools need Python 2):
#
#   WORK=/path/to/work bash abl/build.sh
#
# Result: $WORK/abl/out/abl.elf. XBL wants the Qualcomm hash segment on
# it: with QTESTSIGN=/path/to/qtestsign.py (github msm8916-mainline/
# qtestsign) and STOCK_ABL=<stock abl_a> it also writes $WORK/abl/out/abl-pro1x.mbn. The
# Pro1X has secure boot off ("getvar secure: no"), so a test chain does.
# Flashing: abl/README.md.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:?set WORK=<work directory>}
TAG=LA.UM.9.15.r1-05900-KAMORTA.QSSI12.0
D=$WORK/abl
mkdir -p "$D"
if [ ! -d "$D/edk2" ]; then
	git clone -q --depth 1 --branch "$TAG" \
	    https://git.codelinaro.org/clo/la/abl/tianocore/edk2.git "$D/edk2"
fi
(
	cd "$D/edk2"
	git reset -q --hard "$TAG"
	git clean -qfdx
	git -c user.name=pro1x -c user.email=pro1x@localhost am -q "$HERE"/patches/*.patch
)
cp "$HERE/build-in-container.sh" "$HERE/hostcc" "$HERE/hostcxx" "$D/"
mkdir -p "$D/out"
docker build -q -t pro1x-abl-build "$HERE" > /dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$D:/src" pro1x-abl-build \
    /src/build-in-container.sh
if [ -n "${QTESTSIGN:-}" ]; then
	python3 "$QTESTSIGN" -v6 abl "$D/out/abl.elf" \
	    -o "$D/out/abl-test-signed.mbn"
	# qtestsign leaves the v6 metadata zero; XBL wants ABL's SW_ID there,
	# taken from the stock ABL (STOCK_ABL, read from abl_a in EDL)
	python3 "$HERE/setmeta.py" "$D/out/abl-test-signed.mbn" \
	    "${STOCK_ABL:?set STOCK_ABL=<the stock abl_a image>}" \
	    "$D/out/abl-pro1x.mbn"
	ls -la "$D/out/abl-pro1x.mbn"
fi
