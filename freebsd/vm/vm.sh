#!/bin/bash
# A FreeBSD/arm64 machine on the PC (qemu, emulated): where the packages are
# made (hildon/package.sh: FreeBSD's pkg create and pkg repo) and the
# desktop and its apps are tried on a virtual screen before they go to the
# phone or into a release.
#
#   VMDIR=~/freebsd-vm bash freebsd/vm/vm.sh setup   once: image, key, seed
#   VMDIR=~/freebsd-vm bash freebsd/vm/vm.sh run     start it (serial console;
#                                                    AUDIO=wav: a sound card
#                                                    whose output goes to
#                                                    $VMDIR/vm-audio.wav, which
#                                                    grows while the VM runs;
#                                                    in the VM: kldload snd_hda)
#   VMDIR=~/freebsd-vm bash freebsd/vm/vm.sh ssh [command]
#
# Needs qemu-system-aarch64 and edk2-aarch64 (Arch: pacman), genisoimage.
# The release's VM image (SHA-256 from its CHECKSUM file) with cloud-init:
# root logs in with the key made here, at 127.0.0.1:2222.
set -euo pipefail
VMDIR=${VMDIR:?set VMDIR=<directory for the VM>}
REL=${REL:-15.1-RELEASE}
IMG=FreeBSD-$REL-arm64-aarch64-BASIC-CLOUDINIT-ufs.qcow2
BASE=https://download.freebsd.org/releases/VM-IMAGES/$REL/aarch64/Latest
FW=/usr/share/edk2/aarch64
PORT=${PORT:-2222}

case ${1:-} in
setup)
	mkdir -p "$VMDIR/seed"
	cd "$VMDIR"
	curl -sSfL -o CHECKSUM.SHA256 "$BASE/CHECKSUM.SHA256"
	curl -sSfL -o "$IMG.xz" "$BASE/$IMG.xz"
	sum=$(sed -n "s/^SHA256 ($IMG.xz) = //p" CHECKSUM.SHA256)
	echo "$sum  $IMG.xz" | sha256sum -c -
	xz -d "$IMG.xz"
	mv "$IMG" disk.qcow2
	qemu-img resize disk.qcow2 +14G
	ssh-keygen -q -t ed25519 -N "" -C freebsd-vm -f vm_key
	k=$(cat vm_key.pub)
	printf 'instance-id: freebsd-vm-1\nlocal-hostname: freebsd-vm\n' > seed/meta-data
	cat > seed/user-data <<-UD
	#cloud-config
	ssh_authorized_keys:
	  - $k
	runcmd:
	  - mkdir -p /root/.ssh && chmod 700 /root/.ssh
	  - echo '$k' > /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys
	  - echo 'PermitRootLogin prohibit-password' >> /etc/ssh/sshd_config
	  - sysrc sshd_enable=YES
	  - service sshd restart
	UD
	genisoimage -quiet -output seed.iso -volid cidata -joliet -rock \
	    seed/user-data seed/meta-data
	cp "$FW/QEMU_VARS.fd" vars.fd
	echo "set up in $VMDIR; first start updates the base system (slow)"
	;;
run)
	cd "$VMDIR"
	audio=()
	[ "${AUDIO:-}" != wav ] || audio=(-audiodev wav,id=snd0,path=vm-audio.wav
	    -device intel-hda -device hda-output,audiodev=snd0)
	exec qemu-system-aarch64 "${audio[@]}" -M virt -cpu max -smp 4 -m 6144 \
	    -accel tcg,thread=multi \
	    -drive if=pflash,format=raw,readonly=on,file="$FW/QEMU_EFI.fd" \
	    -drive if=pflash,format=raw,file=vars.fd \
	    -drive if=virtio,format=qcow2,file=disk.qcow2 \
	    -device virtio-scsi-pci \
	    -drive if=none,id=seed,format=raw,media=cdrom,file=seed.iso \
	    -device scsi-cd,drive=seed \
	    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
	    -device virtio-net-pci,netdev=n0 -nographic -serial mon:stdio
	;;
ssh)
	shift
	exec ssh -i "$VMDIR/vm_key" -p "$PORT" \
	    -o UserKnownHostsFile="$VMDIR/known_hosts" \
	    -o StrictHostKeyChecking=accept-new -o BatchMode=yes root@127.0.0.1 "$@"
	;;
*)
	sed -n '2,13s/^# \{0,1\}//p' "$0" >&2
	exit 2
	;;
esac
