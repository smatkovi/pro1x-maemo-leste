#!/usr/bin/python3
# MCE's get_device_mode on the system bus, the one MCE call liblocation
# makes before its dialogs: "normal" (or the mode given), so a chroot
# without MCE gets past liblocation's device mode check.
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


class Request(dbus.service.Object):
    def __init__(self, bus, mode):
        self.mode = mode
        name = dbus.service.BusName("com.nokia.mce", bus)
        super().__init__(name, "/com/nokia/mce/request")

    @dbus.service.method("com.nokia.mce.request", out_signature="s")
    def get_device_mode(self):
        print("get_device_mode:", self.mode, flush=True)
        return self.mode


DBusGMainLoop(set_as_default=True)
service = Request(dbus.SystemBus(), sys.argv[1] if len(sys.argv) > 1 else "normal")
GLib.MainLoop().run()
