#!/usr/bin/env python3
"""Reboot straight into the bootloader (fastboot) -- run as root.

Works on both systems (Sailfish and Maemo Leste), because it calls the
reboot(2) syscall with LINUX_REBOOT_CMD_RESTART2 and the argument "bootloader".
The Qualcomm reboot handler in the kernel turns that into the reboot reason
that the ABL evaluates on the next start.

The usual ways via `reboot bootloader` tend to fail: busybox reboot takes no
argument, and `systemctl reboot bootloader` only exists from systemd 246 on.
"""
import ctypes
import os
import sys

MAGIC1 = 0xFEE1DEAD
MAGIC2 = 0x28121969
CMD_RESTART2 = 0xA1B2C3D4
NR_REBOOT_AARCH64 = 142

if os.geteuid() != 0:
    sys.exit("run as root")

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
# On success the call does not return.
err = ctypes.get_errno()
sys.exit(f"reboot syscall failed: rc={rc} errno={err} ({os.strerror(err)})")
