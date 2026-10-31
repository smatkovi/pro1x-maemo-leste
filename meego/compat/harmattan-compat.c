/*
 * Small differences between the N9 and the Pro1X that Harmattan programs
 * stumble over. Loaded into every Harmattan process through
 * /etc/ld.so.preload (38-hardware-adaptation.sh); each override only takes
 * effect in the program that calls the function through its PLT.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXPORT __attribute__((visibility("default")))

/*
 * MafwMmcMonitor::isMounted() - the media player (qmafw's GStreamer
 * renderer) plays nothing from /home/user/MyDocs while it is not mounted:
 * on the N9 that was its own FAT partition, unmounted for USB mass
 * storage. It looks for a GIO mount there, and GIO ignores bind mounts.
 * On the Pro1X MyDocs is a directory and always there.
 */
EXPORT bool _ZN14MafwMmcMonitor9isMountedEv(void *self)
{
	(void)self;
	return true;
}

/*
 * MeeGo::QmDisplayState's display brightness (qmsystem2; the display
 * settings' slider, QtSystemInfo) - qmsystem2 keeps the level in GConf,
 * /system/osso/dsm/display/display_brightness, where the N9's mce read it.
 * Harmattan's GConf is its own gconfd on the system bus; Leste's mce, which
 * drives the backlight, reads Leste's (the user's gconfd on Leste's session
 * bus) and never saw the setting. So the level (1 to
 * max_display_brightness_levels, 5 on both sides) goes to mce's
 * set_display_brightness and comes from its get_display_brightness, which
 * keep it in Leste's GConf. Harmattan's key is still written (the settings'
 * brief follows it); without an answer from mce, Harmattan's value counts.
 *
 * libdbus and qmsystem2 are Harmattan's, looked up at run time: linked
 * against Debian's libdbus, every process would warn about its missing
 * symbol versions, and qmsystem2's own functions are the ones these
 * replace.
 */
#define DBUS_BUS_SYSTEM 1
#define DBUS_TYPE_INVALID 0
#define DBUS_TYPE_INT32 ((int)'i')
#define MCE_TIMEOUT_MS 1000
#define QM_GET "_ZN5MeeGo14QmDisplayState25getDisplayBrightnessValueEv"
#define QM_SET "_ZN5MeeGo14QmDisplayState25setDisplayBrightnessValueEi"
#define QM_MAX "_ZN5MeeGo14QmDisplayState28getMaxDisplayBrightnessValueEv"

static struct {
	void *(*bus_get_private)(int, void *);
	void (*set_exit_on_disconnect)(void *, uint32_t);
	uint32_t (*get_is_connected)(void *);
	void (*close)(void *);
	void (*unref)(void *);
	void *(*new_method_call)(const char *, const char *, const char *, const char *);
	uint32_t (*append_args)(void *, int, ...);
	void *(*send_with_reply_and_block)(void *, void *, int, void *);
	uint32_t (*get_args)(void *, void *, int, ...);
	void (*message_unref)(void *);
} dbus;

static struct {
	int (*get)(void *);
	void (*set)(void *, int);
	int (*max)(void *);
} qm;

static bool dbus_resolve(void)
{
	static int ok = -1;
	void *lib;

	if (ok >= 0)
		return ok;
	ok = 0;
	lib = dlopen("libdbus-1.so.3", RTLD_NOW);
	if (!lib)
		return false;
	dbus.bus_get_private = dlsym(lib, "dbus_bus_get_private");
	dbus.set_exit_on_disconnect = dlsym(lib, "dbus_connection_set_exit_on_disconnect");
	dbus.get_is_connected = dlsym(lib, "dbus_connection_get_is_connected");
	dbus.close = dlsym(lib, "dbus_connection_close");
	dbus.unref = dlsym(lib, "dbus_connection_unref");
	dbus.new_method_call = dlsym(lib, "dbus_message_new_method_call");
	dbus.append_args = dlsym(lib, "dbus_message_append_args");
	dbus.send_with_reply_and_block = dlsym(lib, "dbus_connection_send_with_reply_and_block");
	dbus.get_args = dlsym(lib, "dbus_message_get_args");
	dbus.message_unref = dlsym(lib, "dbus_message_unref");
	ok = dbus.bus_get_private && dbus.set_exit_on_disconnect && dbus.get_is_connected &&
	     dbus.close && dbus.unref && dbus.new_method_call && dbus.append_args &&
	     dbus.send_with_reply_and_block && dbus.get_args && dbus.message_unref;
	return ok;
}

/* qmsystem2's own functions; it is loaded, the caller links it */
static bool qm_resolve(void)
{
	void *lib;

	if (qm.get && qm.set && qm.max)
		return true;
	lib = dlopen("libqmsystem2.so.1", RTLD_NOW | RTLD_NOLOAD);
	if (!lib)
		return false;
	qm.get = dlsym(lib, QM_GET);
	qm.set = dlsym(lib, QM_SET);
	qm.max = dlsym(lib, QM_MAX);
	return qm.get && qm.set && qm.max;
}

/* mce's get_display_brightness (level 0) or set_display_brightness: the
 * level, -1 without an answer */
static int mce_brightness(const char *method, int level)
{
	static void *bus;
	void *msg, *reply;
	int32_t value = level;

	if (!dbus_resolve())
		return -1;
	if (bus && !dbus.get_is_connected(bus)) {
		dbus.close(bus);
		dbus.unref(bus);
		bus = NULL;
	}
	if (!bus) {
		bus = dbus.bus_get_private(DBUS_BUS_SYSTEM, NULL);
		if (!bus)
			return -1;
		dbus.set_exit_on_disconnect(bus, 0);
	}
	msg = dbus.new_method_call("com.nokia.mce", "/com/nokia/mce/request",
				   "com.nokia.mce.request", method);
	if (!msg)
		return -1;
	if (level > 0)
		dbus.append_args(msg, DBUS_TYPE_INT32, &value, DBUS_TYPE_INVALID);
	reply = dbus.send_with_reply_and_block(bus, msg, MCE_TIMEOUT_MS, NULL);
	dbus.message_unref(msg);
	if (!reply)
		return -1;
	if (level <= 0 && !dbus.get_args(reply, NULL, DBUS_TYPE_INT32, &value, DBUS_TYPE_INVALID))
		value = -1;
	dbus.message_unref(reply);
	return value;
}

EXPORT int _ZN5MeeGo14QmDisplayState25getDisplayBrightnessValueEv(void *self)
{
	int level;

	if (!qm_resolve())
		return -1;
	level = mce_brightness("get_display_brightness", 0);
	if (level >= 1 && level <= qm.max(self))
		return level;
	return qm.get(self);
}

EXPORT void _ZN5MeeGo14QmDisplayState25setDisplayBrightnessValueEi(void *self, int brightness)
{
	if (!qm_resolve())
		return;
	qm.set(self, brightness);
	if (brightness >= 1 && brightness <= qm.max(self))
		mce_brightness("set_display_brightness", brightness);
}
