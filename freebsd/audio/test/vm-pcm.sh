#!/bin/sh
# snd_pro1x.ko in the FreeBSD VM (freebsd/vm, its GENERIC 15.1 kernel with
# its own sound.ko): the driver built with SND_PRO1X_VM (no SMMU, no
# qcom_dsp; sys/modules/snd_pro1x_vm) attached on nexus, then
#   1. /dev/dsp without the daemon: the driver's clock plays and records
#      at the sample rate;
#   2. vm_pcm_test as the daemon with a simulated ADSP: a tone through
#      /dev/dsp arrives in the blocks the "ADSP" reads, recorded blocks
#      arrive at the reader;
#   3. the daemon gone in the middle of a tone: the clock takes over;
#   4. vchans: two players at once;
#   5. the module unloaded.
#
#   VM=<ssh helper> sh freebsd/audio/test/vm-pcm.sh MODULE TEST AUDIOCTL
# (as built into $WORK/audio-out: vm/snd_pro1x.ko, vm/vm_pcm_test,
# bin/pro1x-audioctl). Holds $WORK/.vm-lock while it runs.
set -u
VM=${VM:?ssh helper}
ko=$1 test=$2 ctl=$3
put() { $VM "cat > $2 && chmod 755 $2" < "$1"; }
put "$ko" /tmp/snd_pro1x.ko
put "$test" /tmp/vm_pcm_test
put "$ctl" /tmp/pro1x-audioctl
$VM 'sh -s' <<'EOF'
set -u
t() { echo "$(date +%T) $*"; }
kldunload snd_pro1x 2> /dev/null
kldload sound 2> /dev/null
kenv hw.snd_pro1x.nexus=1 > /dev/null
kldload /tmp/snd_pro1x.ko && echo "snd_pro1x loaded"
dmesg | grep -E '^pcm|snd_pro1x' | tail -4
cat /dev/sndstat
u=$(sed -n 's/^pcm\([0-9]*\): <F(x)tec.*/\1/p' /dev/sndstat | head -1)
echo "unit $u"
sysctl dev.pcm.$u.pro1x
sysctl hw.snd.default_unit=$u > /dev/null
echo "--- 1. the clock alone: 2 s of playback, 1 s of recording"
t start; /tmp/pro1x-audioctl tone 2 440; t end
/tmp/pro1x-audioctl record 1
sysctl dev.pcm.$u.pro1x
echo "--- 2. the daemon with a simulated ADSP"
/tmp/vm_pcm_test 6 > /tmp/vmt.log 2>&1 &
d=$!
sleep 0.5
t "tone 2 s"; /tmp/pro1x-audioctl tone 2 1000; t "tone done"
t "record 1 s"; /tmp/pro1x-audioctl record 1; t "record done"
sleep 1
mixer vol=40 2>&1 | tail -1
mixer rec=line 2>&1 | tail -1
wait $d
cat /tmp/vmt.log
sysctl dev.pcm.$u.pro1x
echo "--- 3. the daemon gone after 20 blocks of a 3 s tone"
/tmp/vm_pcm_test 10 20 > /tmp/vmt.log 2>&1 &
d=$!
sleep 0.5
t "tone 3 s"; /tmp/pro1x-audioctl tone 3 1000; t "tone done"
wait $d
cat /tmp/vmt.log
sysctl dev.pcm.$u.pro1x
echo "--- 4. two players at once (vchans), with the daemon"
/tmp/vm_pcm_test 5 > /tmp/vmt.log 2>&1 &
d=$!
sleep 0.5
/tmp/pro1x-audioctl tone 2 500 &
/tmp/pro1x-audioctl tone 2 700
wait %2 2> /dev/null
wait $d
cat /tmp/vmt.log
sysctl dev.pcm.$u.play.vchans 2>&1
echo "--- 5. unload"
kldunload snd_pro1x && echo "snd_pro1x unloaded"
cat /dev/sndstat
EOF
