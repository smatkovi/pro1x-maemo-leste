#!/usr/bin/python3
# MCE's get_device_mode on the system bus, the one MCE call liblocation
# makes before its dialogs: "normal" (or the mode given), so a chroot
# without MCE gets past liblocation's device mode check. Also a stand-in
# of location-daemon (org.maemo.LocationDaemon), which only prints
# liblocation's "start": with the name owned, the bus does not activate
# the real daemon (which would run gpsd's init script in the chroot).
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


class LocationDaemon(dbus.service.Object):
    def __init__(self, bus):
        name = dbus.service.BusName("org.maemo.LocationDaemon", bus)
        super().__init__(name, "/org/maemo/LocationDaemon")

    @dbus.service.method("org.maemo.LocationDaemon")
    def start(self):
        print("location-daemon: start", flush=True)


DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
service = Request(bus, sys.argv[1] if len(sys.argv) > 1 else "normal")
daemon = LocationDaemon(bus)
GLib.MainLoop().run()
