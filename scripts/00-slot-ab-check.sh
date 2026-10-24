#!/bin/sh
P=/dev/disk/by-partlabel
printf "%-16s %10s  %-10s %-10s %s\n" PART SIZE_MiB A_STATE B_STATE MATCH
for n in xbl xbl_config rpm tz hyp abl bluetooth modem mdtp mdtpsecapp dsp keymaster boot cmnlib cmnlib64 devcfg qupfw vbmeta dtbo imagefv uefisecapp featenabler multiimgoem; do
  a=$P/${n}_a; b=$P/${n}_b
  [ -e "$a" ] && [ -e "$b" ] || { printf "%-16s %10s  %s\n" "$n" "-" "MISSING PAIR"; continue; }
  sz=$(( $(cat /sys/class/block/$(basename $(readlink -f $a))/size) / 2048 ))
  ha=$(dd if=$a bs=1M 2>/dev/null | sha256sum | cut -c1-12)
  hb=$(dd if=$b bs=1M 2>/dev/null | sha256sum | cut -c1-12)
  za=$(dd if=$a bs=1M 2>/dev/null | tr -d '\0' | wc -c)
  zb=$(dd if=$b bs=1M 2>/dev/null | tr -d '\0' | wc -c)
  sa="DATA"; [ "$za" -eq 0 ] && sa="EMPTY"
  sb="DATA"; [ "$zb" -eq 0 ] && sb="EMPTY"
  m="differs"; [ "$ha" = "$hb" ] && m="identical"
  printf "%-16s %10s  %-10s %-10s %s\n" "$n" "$sz" "$sa" "$sb" "$m"
done
