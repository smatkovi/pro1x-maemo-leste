#!/bin/sh
# Rootfs-Image erstmalig auf das Geraet bringen und dort aufziehen.
# Laeuft AUF DEM GERAET als root (zieht per ssh vom Build-Host).
#
# Fuer den ERSTEN Transfer, nicht fuer Updates: das Image ist 24 GiB gross und
# damit per rsync ueber WLAN unzumutbar (rsync liest die volle apparent size,
# gemessen 1,5 MB/s -> gut vier Stunden). Darum wird es auf dem Build-Host mit
# resize2fs -M geschrumpft, komprimiert gestreamt und hier wieder aufgezogen.
# Fuer spaetere Aenderungen: 56-update-rootfs.sh (rsync-Delta).
#
# Rein additiv: es entsteht nur eine neue Datei auf userdata.
set -e
HOST=${HOST:?HOST=user@buildhost setzen}
SRC=${SRC:?SRC=/pfad/out/maemo-leste.img auf dem Build-Host setzen}
DST=${DST:-/userdata/maemo-leste.img}
SIZE_GIB=${SIZE_GIB:-24}
SSH_KEY=${SSH_KEY:-/home/defaultuser/.ssh/id_ed25519}
SSH_KNOWN=${SSH_KNOWN:-/home/defaultuser/.ssh/known_hosts}

# Der Build-Host schrumpft vor dem Senden auf Mindestgroesse.
echo ">> schrumpfe Quelle auf $HOST"
ssh -o BatchMode=yes -i "$SSH_KEY" -o UserKnownHostsFile="$SSH_KNOWN" "$HOST" "
  set -e
  e2fsck -fp '$SRC' || true
  resize2fs -M '$SRC' >/dev/null
  blocks=\$(dumpe2fs -h '$SRC' 2>/dev/null | awk -F: '/Block count/{gsub(/ /,\"\",\$2); print \$2}')
  bs=\$(dumpe2fs -h '$SRC' 2>/dev/null | awk -F: '/Block size/{gsub(/ /,\"\",\$2); print \$2}')
  truncate -s \$((blocks * bs)) '$SRC'
  ls -la '$SRC'"

echo ">> uebertrage $HOST:$SRC -> $DST (zstd-komprimiert)"
ssh -o BatchMode=yes -i "$SSH_KEY" -o UserKnownHostsFile="$SSH_KNOWN" "$HOST" \
  "zstd -c -T0 -3 '$SRC'" | zstd -dc > "$DST"

# Achtung: busybox auf Sailfish hat kein truncate, darum python3.
echo ">> auf ${SIZE_GIB} GiB aufziehen"
python3 -c "open('$DST','r+b').truncate($SIZE_GIB*1024**3)"
e2fsck -fp "$DST" || true
resize2fs "$DST"

# Gleiche Geometrie auf beiden Seiten, damit 56-update-rootfs.sh spaeter
# wirklich nur Deltas uebertragen kann.
echo ">> Quelle auf $HOST ebenfalls auf ${SIZE_GIB} GiB bringen"
ssh -o BatchMode=yes -i "$SSH_KEY" -o UserKnownHostsFile="$SSH_KNOWN" "$HOST" "
  set -e
  truncate -s ${SIZE_GIB}G '$SRC'
  e2fsck -fp '$SRC' || true
  resize2fs '$SRC' >/dev/null
  echo quelle-fertig"

echo ">> Ergebnis"
dumpe2fs -h "$DST" 2>/dev/null | grep -E 'Filesystem volume name|Block count|Free blocks|Filesystem state'
ls -la "$DST"
