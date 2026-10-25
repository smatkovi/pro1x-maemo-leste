/*
 * radiostates.c - Harmattan's radio states on Leste's mce
 *
 * Harmattan switches flight mode through mce's radio states (qmsystem2's
 * QmDeviceMode, the status menu and the settings): a bit mask with the
 * master switch in bit 0. Fremantle and Leste switch it through the device
 * mode ("normal"/"flight"), which icd2 and the other Leste services
 * follow. This module translates between the two:
 *
 *   get_radio_states                   -> all radios, or none in flight mode
 *   req_radio_states_change(u, u mask) -> the master bit sets the device mode
 *   radio_states_ind (u)               <- sent when the device mode changes
 *
 * The single radios (cellular, WLAN, Bluetooth, NFC, FM transmitter) are
 * not switched one by one.
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
#define MCE_RADIO_STATES_ALL		0x3f	/* master, cellular, WLAN, BT, NFC, FM TX */

static const gchar *const provides[] = { MODULE_NAME, NULL };

G_MODULE_EXPORT module_info_struct module_info = {
	.name = MODULE_NAME,
	.provides = provides,
	.priority = 250
};

static gconstpointer get_handler, change_handler;

static dbus_uint32_t radio_states(void)
{
	return mce_get_device_mode_int32() == MCE_FLIGHT_MODE_INT32 ? 0 : MCE_RADIO_STATES_ALL;
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
	dbus_uint32_t states = 0, mask = 0;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32, &states,
				   DBUS_TYPE_UINT32, &mask, DBUS_TYPE_INVALID)) {
		mce_log(LL_ERR, "%s: bad %s: %s", MODULE_NAME,
			MCE_RADIO_STATES_CHANGE_REQ, error.message);
		dbus_error_free(&error);
		return FALSE;
	}

	if (mask & MCE_RADIO_STATE_MASTER) {
		device_mode_t mode = (states & MCE_RADIO_STATE_MASTER) ?
			MCE_NORMAL_MODE_INT32 : MCE_FLIGHT_MODE_INT32;
		mce_log(LL_DEBUG, "%s: device mode %d", MODULE_NAME, mode);
		mce_set_device_mode_int32(mode);
	}

	if (!dbus_message_get_no_reply(msg))
		return dbus_send_message(dbus_new_method_reply(msg));
	return TRUE;
}

static void mode_trigger(gconstpointer data)
{
	(void)data;
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
