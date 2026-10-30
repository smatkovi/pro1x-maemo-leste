# Common parts of the device-day helpers (docs/device-day-2026-10-31.md).
# Sourced by deploy.sh, collect.sh, backup.sh and phone.sh; runs on the PC.
#
# Reaching a system on the phone: SYS (leste, meego, kali, arch, luneos,
# freebsd; n950 for the Nokia N950) picks the user, the address and the
# known hosts file. Every system has host keys of its own on the same
# address (172.16.42.1), so each has its own file, as 68-power-standby.sh
# and freebsd/pm/README.md do: $KNOWN_DIR/known_hosts.pro1x-<sys>. ssh runs
# with StrictHostKeyChecking=yes; a system's first key is pinned once with
# `phone.sh pin <sys>` (accept-new, into that system's file only).
#
# Testing on the PC (tools/devday/test/run.sh): SSH and SCP name stand-ins,
# PHONE_ROOT is the fake phone's root directory (every phone path the
# helpers use starts with $R, which is PHONE_ROOT, empty on the phone).
# DRY=1 prints what would be run on the phone instead of running it.

DAY=${DAY:-2026-10-31}
WORK=${WORK:-/run/media/sebastian/2e638a7f-26db-4e89-9446-81d688464798/pro1x-leste}
REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
KOS=${KOS:-$HOME/ps/kolibrios-arm}
KNOWN_DIR=${KNOWN_DIR:-$HOME/.ssh}
RUNS=${RUNS:-$HOME/ps/pro1x-runs/$DAY}
SSH=${SSH:-ssh}
SCP=${SCP:-scp}
DRY=${DRY:-0}
PHONE_ROOT=${PHONE_ROOT:-}
SYS=${SYS:-leste}
USBIF=${USBIF:-enp19s0u1u1}
# the free space every disk of the PC keeps (GiB)
MIN_FREE_GB=${MIN_FREE_GB:-10}

log() { echo ">> $*" >&2; }
die() { echo "error: $*" >&2; exit 1; }

# sys_user / sys_host / sys_sudo SYS
sys_user() {
	case $1 in
	kali) echo kali ;;
	arch) echo alarm ;;
	*) echo root ;;
	esac
}
sys_host() {
	case $1 in
	n950) echo "${N950_IP:-192.168.2.15}" ;;
	*) echo "${PHONE_IP:-172.16.42.1}" ;;
	esac
}
# root's commands: Arch's alarm has sudo without a password (as in
# 68-power-standby.sh); Kali's kali asks for one, so there they run as kali
# (dmesg may then be refused: collect what needs root by hand with sudo)
sys_sudo() {
	case $1 in
	arch) echo "sudo -n" ;;
	*) echo "" ;;
	esac
}
known_file() {
	# the MeeGo copy is a copy of Leste's image: Leste's keys unless it
	# got a file of its own
	if [ "$1" = meego ] && [ ! -f "$KNOWN_DIR/known_hosts.pro1x-meego" ]; then
		echo "$KNOWN_DIR/known_hosts.pro1x-leste"
	else
		echo "$KNOWN_DIR/known_hosts.pro1x-$1"
	fi
}

# The ssh options for SYS. A known hosts file with only an RSA key (Leste's
# pin from 2026-10-25) needs the RSA signature algorithms offered first.
ssh_opts() {
	local kf
	kf=$(known_file "$1")
	printf '%s\n' -o BatchMode=yes -o ConnectTimeout="${CONNECT_TIMEOUT:-10}" \
	    -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$kf"
	if [ -f "$kf" ] && grep -q ' ssh-rsa ' "$kf" &&
	    ! grep -q -E ' (ssh-ed25519|ecdsa-sha2-[a-z0-9]+) ' "$kf"; then
		printf '%s\n' -o HostKeyAlgorithms=rsa-sha2-512,rsa-sha2-256
	fi
}

# ph CMD: CMD as root (sudo -n for kali and arch) in SYS on the phone, with
# R set to PHONE_ROOT in front. stdin goes through.
ph() {
	local opts target cmd
	mapfile -t opts < <(ssh_opts "$SYS")
	target=$(sys_user "$SYS")@$(sys_host "$SYS")
	cmd="R='$PHONE_ROOT'; $*"
	if [ -n "$(sys_sudo "$SYS")" ]; then
		cmd="$(sys_sudo "$SYS") sh -c $(printf '%q' "$cmd")"
	fi
	if [ "$DRY" = 1 ]; then
		echo "[dry] $SSH $target: $*" >&2
		return 0
	fi
	$SSH "${opts[@]}" "$target" "$cmd"
}

# put LOCAL REMOTE: scp LOCAL to REMOTE (a phone path below $R)
put() {
	local opts target
	mapfile -t opts < <(ssh_opts "$SYS")
	target=$(sys_user "$SYS")@$(sys_host "$SYS")
	if [ "$DRY" = 1 ]; then
		echo "[dry] $SCP $1 -> $target:$2" >&2
		return 0
	fi
	$SCP -q "${opts[@]}" "$1" "$target:$PHONE_ROOT$2"
}

# free_gb DIR: free space of DIR's file system in GiB (whole numbers)
free_gb() { df -Pk "$1" | awk 'NR == 2 { print int($4 / 1048576) }'; }

# need_space DIR MB: refuse when DIR would keep less than MIN_FREE_GB
need_space() {
	local d=$1 mb=$2 free
	mkdir -p "$d"
	free=$(df -Pk "$d" | awk 'NR == 2 { print int($4 / 1024) }')
	[ $((free - mb)) -ge $((MIN_FREE_GB * 1024)) ] ||
	    die "$d: $free MB free, $mb MB needed and $MIN_FREE_GB GiB to keep"
	log "$d: $free MB free, $mb MB needed, $MIN_FREE_GB GiB kept"
}

sha() { sha256sum < "$1" | cut -c1-64; }
