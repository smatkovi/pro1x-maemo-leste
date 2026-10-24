#!/bin/sh
# Rootfs-Image auf das Geraet bringen und dort wieder aufziehen.
# Laeuft AUF DEM GERAET als root (zieht per ssh vom Build-Host).
#
# Das Image wird auf dem Build-Host mit resize2fs -M geschrumpft uebertragen
# (24 GiB sparse ueber WLAN waeren Stunden), hier wieder auf Zielgroesse
# gebracht. Rein additiv: es entsteht nur eine neue Datei auf userdata.
set -e
HOST=${HOST:?HOST=user@buildhost setzen}
SRC=${SRC:?SRC=/pfad/out/maemo-leste.img auf dem Build-Host setzen}
DST=${DST:-/userdata/maemo-leste.img}
SIZE=${SIZE:-24G}
SSH_KEY=${SSH_KEY:-/home/defaultuser/.ssh/id_ed25519}

echo ">> uebertrage $HOST:$SRC -> $DST (zstd-komprimiert)"
ssh -o BatchMode=yes -i "$SSH_KEY" "$HOST" "zstd -c -T0 -3 $SRC" | zstd -dc > "$DST"

echo ">> auf $SIZE aufziehen"
truncate -s "$SIZE" "$DST"
e2fsck -fp "$DST" || true
resize2fs "$DST"

echo ">> Ergebnis"
dumpe2fs -h "$DST" 2>/dev/null | grep -E 'Filesystem volume name|Block count|Free blocks'
ls -la "$DST"
