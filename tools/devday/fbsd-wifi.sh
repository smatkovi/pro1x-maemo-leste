#!/bin/sh
# FreeBSD on the Pro1X, until the root image starts the Wi-Fi itself: the
# modem with its servers (the WLAN firmware runs on it), then ath10k and
# wlan0 joining the network of userdata's wpa_supplicant.conf
# (freebsd/dsp/README.md, runs dsp6 and dsp7), as rc.conf's commented
# lines would. Each step's output, the kernel's lines on it and the
# address it got go to /tmp/pro1x-wifi.log, which fbsd-x.sh keeps in
# /dev/pro1xshot.
#
#   sh /dev/pro1x-host/freebsd/wifi.sh    (as root; fbsd-x.sh starts it in
#                                          the background when the cmdline
#                                          has pro1x.wifi=on)
L=/tmp/pro1x-wifi.log
{
	echo "=== pro1x wifi.sh $(date)"
	# started already (rc.conf, or this script before): only the report
	services="tqftpserv pd_mapper rmtfs pro1x_modem pro1x_wifi"
	if service pro1x_wifi onestatus > /dev/null 2>&1; then
		echo "--- running already"
		services=
	fi
	for s in $services; do
		echo "--- service $s onestart"
		service $s onestart 2>&1
		echo "(exit $?)"
	done
	# wpa_supplicant and dhclient from pro1x_wifi's startchildren
	i=0
	until ifconfig wlan0 2> /dev/null | grep -q 'inet ' || [ $i -ge 60 ]; do
		sleep 2
		i=$((i + 2))
	done
	echo "--- after $i s"
	ifconfig wlan0 2>&1
	netstat -rn -f inet 2>&1 | head -8
	cat /etc/resolv.conf 2>&1
	gw=$(route -n get default 2> /dev/null | awk '$1 == "gateway:" { print $2 }')
	[ -n "$gw" ] && ping -c 3 -t 10 "$gw" 2>&1
	echo "--- the kernel"
	dmesg | grep -E 'ath10k|qcom_(pas|smmu|rpm|glink)|qrtr|wlan0|modem' | tail -n 80
	sysctl net.qrtr.servers 2>&1
} > $L 2>&1
cat $L
