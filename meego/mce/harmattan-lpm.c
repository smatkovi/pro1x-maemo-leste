/*
 * harmattan-lpm.c - Harmattan's low power mode view on Leste's mce
 *
 * In low power mode (patches/lpm) mce does not blank the display but keeps
 * the panel at low brightness in its own low power mode (MCE_DISPLAY_LPM_ON)
 * and says "lpm" in display_status_ind, for a stand-by screen to draw; on
 * Leste pro1x-standby does that. Harmattan's system UI (sysuid) has its own
 * stand-by screen, the lock screen's low power mode view (the time and the
 * notifications), which Harmattan's mce opened with tklock_open's mode
 * TKLOCK_ENABLE_LPM_UI and paused with TKLOCK_PAUSE_UI when the display went
 * dark in low power mode (tklock.c, open_tklock_ui()). Leste's lock-tklock
 * opens only the lock screen itself, so in the MeeGo boot the panel showed
 * the plain lock screen, dimmed. This module adds the calls, while locked:
 *
 *   to LPM_ON                    -> tklock_open(TKLOCK_ENABLE_LPM_UI)
 *   LPM_ON to LPM_OFF            -> tklock_open(TKLOCK_PAUSE_UI)
 *   low power mode to on or dim  -> tklock_open(TKLOCK_ENABLE), the lock
 *                                   screen without the low power mode view
 *
 * with lock-tklock's callback, so that unlocking still reaches it. Loaded
 * only in the MeeGo boot (mce.ini.d/60-pro1x.ini): Hildon's system UI does
 * not know these modes.
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 */

#include <glib.h>
#include <gmodule.h>
#include <systemui/dbus-names.h>
#include <systemui/tklock-dbus-names.h>
#include "mce.h"
#include "mce-log.h"
#include "mce-dbus.h"
#include "datapipe.h"

#define MODULE_NAME "harmattan-lpm"

/* lock-tklock's callback (lock-tklock.c) */
#define MCE_TKLOCK_CB_REQ "tklock_callback"

static const gchar *const provides[] = { MODULE_NAME, NULL };

G_MODULE_EXPORT module_info_struct module_info = {
	.name = MODULE_NAME,
	.provides = provides,
	/* not used for the order: ModulesUser loads after Modules, so this
	 * trigger runs after lock-tklock's, which locks on the way into low
	 * power mode */
	.priority = 300
};

static void open_tklock_ui(dbus_uint32_t mode)
{
	const gchar *const cb_service = MCE_SERVICE;
	const gchar *const cb_path = MCE_REQUEST_PATH;
	const gchar *const cb_interface = MCE_REQUEST_IF;
	const gchar *const cb_method = MCE_TKLOCK_CB_REQ;
	dbus_bool_t silent = TRUE;
	dbus_bool_t flicker_key = FALSE;

	mce_log(LL_DEBUG, "%s: tklock_open mode %u", MODULE_NAME, mode);
	/* without waiting for sysuid's answer, as lock-tklock waits for its own */
	if (!dbus_send(SYSTEMUI_SERVICE, SYSTEMUI_REQUEST_PATH, SYSTEMUI_REQUEST_IF,
		       SYSTEMUI_TKLOCK_OPEN_REQ, NULL,
		       DBUS_TYPE_STRING, &cb_service,
		       DBUS_TYPE_STRING, &cb_path,
		       DBUS_TYPE_STRING, &cb_interface,
		       DBUS_TYPE_STRING, &cb_method,
		       DBUS_TYPE_UINT32, &mode,
		       DBUS_TYPE_BOOLEAN, &silent,
		       DBUS_TYPE_BOOLEAN, &flicker_key,
		       DBUS_TYPE_INVALID))
		mce_log(LL_ERR, "%s: tklock_open (mode %u) failed", MODULE_NAME, mode);
}

static void display_state_trigger(gconstpointer data)
{
	static display_state_t old_state = MCE_DISPLAY_UNDEF;
	display_state_t state = GPOINTER_TO_INT(data);
	gboolean locked = (mce_get_submode_int32() & MCE_TKLOCK_SUBMODE) != 0;
	gboolean was_lpm = old_state == MCE_DISPLAY_LPM_ON || old_state == MCE_DISPLAY_LPM_OFF;

	if (state == old_state)
		return;

	if (locked) {
		if (state == MCE_DISPLAY_LPM_ON)
			open_tklock_ui(TKLOCK_ENABLE_LPM_UI);
		else if (state == MCE_DISPLAY_LPM_OFF && old_state == MCE_DISPLAY_LPM_ON)
			open_tklock_ui(TKLOCK_PAUSE_UI);
		else if ((state == MCE_DISPLAY_ON || state == MCE_DISPLAY_DIM) && was_lpm)
			open_tklock_ui(TKLOCK_ENABLE);
	}
	old_state = state;
}

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	(void)module;

	append_output_trigger_to_datapipe(&display_state_pipe, display_state_trigger);
	return NULL;
}

G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	remove_output_trigger_from_datapipe(&display_state_pipe, display_state_trigger);
}
