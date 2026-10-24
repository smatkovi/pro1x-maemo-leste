#!/bin/sh
# Rootfs-Image auf dem Geraet aktualisieren -- per rsync-Delta.
# Laeuft AUF DEM GERAET als root.
#
# Hier ist rsync das richtige Werkzeug: nach dem Erstdeployment liegt auf
# beiden Seiten dasselbe 24-GiB-Image, und geaendert hat sich meist nur ein
# Bruchteil (neuer Kernel, neue Module, ein paar Pakete). Mit --inplace und
# --no-whole-file ueberträgt rsync nur die abweichenden Bloecke statt der
# ganzen Datei.
#
# Voraussetzung: beide Seiten haben dieselbe Dateigroesse/Geometrie
# (55-deploy-rootfs.sh stellt das her). Ohne --inplace wuerde rsync eine
# 24-GiB-Kopie daneben anlegen -- dafuer ist auf userdata zwar Platz, es ist
# aber langsamer und unnoetig.
set -e
HOST=${HOST:?HOST=user@buildhost setzen}
SRC=${SRC:?SRC=/pfad/out/maemo-leste.img auf dem Build-Host setzen}
DST=${DST:-/userdata/maemo-leste.img}
SSH_KEY=${SSH_KEY:-/home/defaultuser/.ssh/id_ed25519}
SSH_KNOWN=${SSH_KNOWN:-/home/defaultuser/.ssh/known_hosts}

[ -f "$DST" ] || { echo "$DST fehlt -- erst 55-deploy-rootfs.sh"; exit 1; }

# Das Image darf waehrend des Updates nicht eingehaengt sein.
if grep -q "$DST" /proc/mounts 2>/dev/null; then
  echo "$DST ist gemountet -- erst aushaengen"; exit 1
fi
for l in $(ls /sys/block/loop*/loop/backing_file 2>/dev/null); do
  if [ "$(cat "$l" 2>/dev/null)" = "$DST" ]; then
    echo "$DST haengt an einem Loop-Device -- erst loeschen (losetup -d)"; exit 1
  fi
done

echo ">> rsync-Delta $HOST:$SRC -> $DST"
rsync -a --inplace --no-whole-file --info=progress2 \
  -e "ssh -o BatchMode=yes -i $SSH_KEY -o UserKnownHostsFile=$SSH_KNOWN" \
  "$HOST:$SRC" "$DST"

e2fsck -fp "$DST" || true
dumpe2fs -h "$DST" 2>/dev/null | grep -E 'Block count|Free blocks|Filesystem state'
