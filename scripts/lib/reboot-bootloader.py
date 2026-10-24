#!/usr/bin/env python3
"""Reboot direkt in den Bootloader (fastboot) -- als root ausfuehren.

Funktioniert auf beiden Systemen (Sailfish und Maemo Leste), weil es den
reboot(2)-Syscall mit LINUX_REBOOT_CMD_RESTART2 und dem Argument "bootloader"
aufruft. Der Qualcomm-Reboot-Handler im Kernel legt daraus die Reboot-Reason
ab, die der ABL beim naechsten Start auswertet.

Die Wege ueber `reboot bootloader` scheitern sonst gern: busybox-reboot nimmt
kein Argument, und `systemctl reboot bootloader` gibt es erst ab systemd 246.
"""
import ctypes
import os
import sys

MAGIC1 = 0xFEE1DEAD
MAGIC2 = 0x28121969
CMD_RESTART2 = 0xA1B2C3D4
NR_REBOOT_AARCH64 = 142

if os.geteuid() != 0:
    sys.exit("als root ausfuehren")

arg = (sys.argv[1] if len(sys.argv) > 1 else "bootloader").encode()

libc = ctypes.CDLL("libc.so.6", use_errno=True)
libc.sync()
print(f"reboot -> {arg.decode()!r}", flush=True)

rc = libc.syscall(
    ctypes.c_long(NR_REBOOT_AARCH64),
    ctypes.c_ulong(MAGIC1),
    ctypes.c_ulong(MAGIC2),
    ctypes.c_ulong(CMD_RESTART2),
    ctypes.c_char_p(arg),
)
# Bei Erfolg kommt der Aufruf nicht zurueck.
err = ctypes.get_errno()
sys.exit(f"reboot-Syscall fehlgeschlagen: rc={rc} errno={err} ({os.strerror(err)})")
