#!/bin/sh
# On the phone, as root in Leste (or the MeeGo copy): the packages
# tools/devday/fetch.sh fetched and deploy.sh leste-debs copied to
# /root/devday/debs/<group>/, group by group in a safe order:
#   1-gtk 2-libs 3-system 4-apps 5-harmattan
# (libraries before what uses them, the user interface's daemons before
# the applications; ofono and waydroid have blocks of their own).
#
#   sh leste-install.sh plan          what would be installed, nothing else
#   sh leste-install.sh install       install
#
# Only what is installed already and older than the package is upgraded;
# NEW=1 also installs packages that are not there (e.g. the N9 look's
# harmattan-* in the MeeGo copy). PKG_GROUPS="2-libs 4-apps" limits the
# groups.
# Installed packages are held (apt-mark hold), as scripts/49-*.sh do, so a
# Leste upgrade does not replace them. No service is restarted while
# installing (a policy-rc.d for the time of it: restarting mce or
# hildon-desktop breaks the display's state): reboot afterwards.
# Logs to /root/devday/leste-install-<time>.log.
set -eu
DEBS=${DEBS:-/root/devday/debs}
PKG_GROUPS=${PKG_GROUPS:-1-gtk 2-libs 3-system 4-apps 5-harmattan}
NEW=${NEW:-0}
mode=${1:-plan}
case $mode in plan|install) ;; *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
LOG=/root/devday/leste-install-$(date +%Y%m%d-%H%M%S).log
mkdir -p /root/devday
say() { echo "$*" | tee -a "$LOG"; }

total=0
for g in $PKG_GROUPS; do
	[ -d "$DEBS/$g" ] || continue
	list=
	for d in "$DEBS/$g"/*.deb; do
		[ -f "$d" ] || continue
		p=$(dpkg-deb -f "$d" Package)
		v=$(dpkg-deb -f "$d" Version)
		have=$(dpkg-query -W -f '${db:Status-Abbrev} ${Version}' "$p" 2> /dev/null || true)
		case $have in
		ii*)	hv=${have##* }
			if dpkg --compare-versions "$v" gt "$hv"; then
				say "$g: $p $hv -> $v"
				list="$list $d"
			else
				say "$g: $p $hv (have it; package $v)"
			fi ;;
		*)	if [ "$NEW" = 1 ]; then
				say "$g: $p new $v"
				list="$list $d"
			else
				say "$g: $p not installed (NEW=1 installs it)"
			fi ;;
		esac
	done
	[ -n "$list" ] || continue
	total=$((total + 1))
	[ "$mode" = install ] || continue
	made=
	if [ ! -e /usr/sbin/policy-rc.d ]; then
		printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
		chmod 755 /usr/sbin/policy-rc.d
		made=1
	fi
	say "== installing $g:$list"
	# shellcheck disable=SC2086
	if ! apt-get install -y $list >> "$LOG" 2>&1; then
		[ -z "$made" ] || rm -f /usr/sbin/policy-rc.d
		say "FAILED in $g (see $LOG); the groups after it are not installed"
		exit 1
	fi
	[ -z "$made" ] || rm -f /usr/sbin/policy-rc.d
	for d in $list; do apt-mark hold "$(dpkg-deb -f "$d" Package)" >> "$LOG"; done
done
if [ "$mode" = plan ]; then
	say "plan: $total group(s) with something to install; run: sh $0 install"
else
	say "done; reboot for the new daemons (mce, hildon-desktop) to start"
fi
