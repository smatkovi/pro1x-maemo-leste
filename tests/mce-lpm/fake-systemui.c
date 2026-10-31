/* A stand-in for the system UI on a private bus: owns com.nokia.system_ui,
 * answers every request with int32 0 and logs tklock_open's mode */
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
	DBusError e;
	DBusConnection *c;

	dbus_error_init(&e);
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!(c = dbus_bus_get(DBUS_BUS_SYSTEM, &e))) {
		fprintf(stderr, "bus: %s\n", e.message);
		return 1;
	}
	if (dbus_bus_request_name(c, "com.nokia.system_ui",
	    DBUS_NAME_FLAG_DO_NOT_QUEUE, &e) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "name not ours\n");
		return 1;
	}
	for (;;) {
		DBusMessage *m;

		if (!dbus_connection_read_write(c, -1))
			return 0;
		while ((m = dbus_connection_pop_message(c))) {
			if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
				const char *mem = dbus_message_get_member(m);
				dbus_uint32_t mode = 0;
				struct timespec ts;

				if (!strcmp(mem, "tklock_open")) {
					DBusMessageIter it;

					dbus_message_iter_init(m, &it);
					for (int i = 0; i < 4; i++)
						dbus_message_iter_next(&it);
					if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32)
						dbus_message_iter_get_basic(&it, &mode);
				}
				clock_gettime(CLOCK_REALTIME, &ts);
				printf("%ld.%02ld systemui %s %u\n", (long)ts.tv_sec,
				       ts.tv_nsec / 10000000, mem, mode);
				if (!dbus_message_get_no_reply(m)) {
					DBusMessage *r = dbus_message_new_method_return(m);
					dbus_int32_t v = 0;

					dbus_message_append_args(r, DBUS_TYPE_INT32, &v,
								 DBUS_TYPE_INVALID);
					dbus_connection_send(c, r, NULL);
					dbus_message_unref(r);
				}
			}
			dbus_message_unref(m);
		}
	}
}
