/*
 * radiostates.c - Harmattan's radio states on Leste's mce
 *
 * Harmattan switches flight mode through mce's radio states (qmsystem2's
 * QmDeviceMode, the status menu and the settings): a bit mask with the
 * master switch in bit 0. Fremantle and Leste switch it through the device
 * mode ("normal"/"flight"), which icd2 and the other Leste services
 * follow. This module translates between the two:
 *
 *   get_radio_states                   -> the radios on
 *   req_radio_states_change(u, u mask) -> the master bit sets the device
 *                                         mode, the Bluetooth bit is kept
 *   radio_states_ind (u)               <- sent when either changes
 *
 * Cellular, WLAN, NFC and the FM transmitter follow the device mode: on
 * while online, off in flight mode. Bluetooth is the user's: Harmattan's
 * bluetoothd (bluez 4.91, its maemo6 plugin) powers the adapter as the
 * Bluetooth bit says, at its start and on each radio_states_ind, and asks
 * for the bit when the user switches the adapter. It starts off, as on
 * Leste, and is kept for this boot, one value online and one in flight
 * mode (which starts off), as Harmattan's mce kept radio_states and
 * active_radio_states. Before, the bit read as always set while online
 * and the switch's request for it was dropped: the adapter came up with
 * every session.
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 */

#include <glib.h>
#include <gmodule.h>
#include "mce.h"
#include "mce-log.h"
#include "mce-dbus.h"
#include "datapipe.h"

#define MODULE_NAME "radiostates"

#define MCE_RADIO_STATES_GET		"get_radio_states"
#define MCE_RADIO_STATES_CHANGE_REQ	"req_radio_states_change"
#define MCE_RADIO_STATES_SIG		"radio_states_ind"

#define MCE_RADIO_STATE_MASTER		(1 << 0)
#define MCE_RADIO_STATE_BLUETOOTH	(1 << 3)
#define MCE_RADIO_STATES_ALL		0x3f	/* master, cellular, WLAN, BT, NFC, FM TX */

static const gchar *const provides[] = { MODULE_NAME, NULL };

G_MODULE_EXPORT module_info_struct module_info = {
	.name = MODULE_NAME,
	.provides = provides,
	.priority = 250
};

static gconstpointer get_handler, change_handler;

/* The radios on while online and in flight mode */
static dbus_uint32_t online_states = MCE_RADIO_STATES_ALL & ~MCE_RADIO_STATE_BLUETOOTH;
static dbus_uint32_t offline_states = 0;

static gboolean flight_mode(void)
{
	return mce_get_device_mode_int32() == MCE_FLIGHT_MODE_INT32;
}

static dbus_uint32_t radio_states(void)
{
	return flight_mode() ? offline_states : online_states;
}

static gboolean send_radio_states(DBusMessage *const method_call)
{
	dbus_uint32_t states = radio_states();
	DBusMessage *msg = method_call ? dbus_new_method_reply(method_call) :
		dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF, MCE_RADIO_STATES_SIG);

	if (!dbus_message_append_args(msg, DBUS_TYPE_UINT32, &states, DBUS_TYPE_INVALID)) {
		mce_log(LL_CRIT, "%s: failed to append radio states", MODULE_NAME);
		dbus_message_unref(msg);
		return FALSE;
	}
	return dbus_send_message(msg);
}

static gboolean get_radio_states_dbus_cb(DBusMessage *const msg)
{
	return send_radio_states(msg);
}

static gboolean change_radio_states_dbus_cb(DBusMessage *const msg)
{
	dbus_uint32_t states = 0, mask = 0, old_states;
	dbus_uint32_t *const kept = flight_mode() ? &offline_states : &online_states;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32, &states,
				   DBUS_TYPE_UINT32, &mask, DBUS_TYPE_INVALID)) {
		mce_log(LL_ERR, "%s: bad %s: %s", MODULE_NAME,
			MCE_RADIO_STATES_CHANGE_REQ, error.message);
		dbus_error_free(&error);
		return FALSE;
	}

	old_states = radio_states();
	*kept = (*kept & ~(mask & MCE_RADIO_STATE_BLUETOOTH)) |
		(states & mask & MCE_RADIO_STATE_BLUETOOTH);
	mce_log(LL_DEBUG, "%s: radio states %x", MODULE_NAME, radio_states());

	if (!dbus_message_get_no_reply(msg))
		dbus_send_message(dbus_new_method_reply(msg));
	if (radio_states() != old_states)
		send_radio_states(NULL);

	/* the device mode's change sends them again (mode_trigger) */
	if (mask & MCE_RADIO_STATE_MASTER) {
		device_mode_t mode = (states & MCE_RADIO_STATE_MASTER) ?
			MCE_NORMAL_MODE_INT32 : MCE_FLIGHT_MODE_INT32;
		mce_log(LL_DEBUG, "%s: device mode %d", MODULE_NAME, mode);
		mce_set_device_mode_int32(mode);
	}
	return TRUE;
}

static void mode_trigger(gconstpointer data)
{
	/* flight mode starts with every radio off, as Harmattan's did */
	if (GPOINTER_TO_INT(data) == MCE_FLIGHT_MODE_INT32)
		offline_states = 0;
	send_radio_states(NULL);
}

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	(void)module;

	get_handler = mce_dbus_handler_add(MCE_REQUEST_IF, MCE_RADIO_STATES_GET, NULL,
					   DBUS_MESSAGE_TYPE_METHOD_CALL,
					   get_radio_states_dbus_cb);
	change_handler = mce_dbus_handler_add(MCE_REQUEST_IF, MCE_RADIO_STATES_CHANGE_REQ, NULL,
					      DBUS_MESSAGE_TYPE_METHOD_CALL,
					      change_radio_states_dbus_cb);
	if (!get_handler || !change_handler)
		return "mce_dbus_handler_add failed";

	append_output_trigger_to_datapipe(&mode_pipe, mode_trigger);
	return NULL;
}

G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	remove_output_trigger_from_datapipe(&mode_pipe, mode_trigger);
	if (get_handler)
		mce_dbus_handler_remove(get_handler);
	if (change_handler)
		mce_dbus_handler_remove(change_handler);
}
