/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-modemd's D-Bus side: the part of oFono's API (ofono 2.18,
 * doc/manager-api.txt, modem-api.txt, sim-api.txt, network-api.txt,
 * voicecallmanager-api.txt, voicecall-api.txt, messagemanager-api.txt,
 * message-api.txt) that Maemo Leste's sphone uses (its module comm-ofono:
 * Manager.GetModems, VoiceCallManager.Dial and CallAdded, VoiceCall.Answer,
 * Hangup and PropertyChanged, MessageManager.SendMessage and
 * IncomingMessage), with the properties oFono's other clients read first
 * (Modem, SimManager, NetworkRegistration). Names, signatures and values
 * are oFono's; what is not offered answers org.ofono.Error.NotImplemented.
 *
 * One modem, at M.path. Its interfaces come and go as oFono's atoms do:
 * SimManager and VoiceCallManager with the modem, MessageManager once the
 * SIM is ready, NetworkRegistration once the modem is online; their
 * objects stay registered, Modem.Interfaces says which are usable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modemd.h"

#define	OFONO_SERVICE	"org.ofono"

static GDBusConnection *bus;
static GDBusNodeInfo *node_info;

static const char introspection[] =
    "<node>"
    "<interface name='org.ofono.Manager'>"
    " <method name='GetModems'><arg type='a(oa{sv})' direction='out'/></method>"
    " <signal name='ModemAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
    " <signal name='ModemRemoved'><arg type='o'/></signal>"
    "</interface>"
    "<interface name='org.ofono.Modem'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    "</interface>"
    "<interface name='org.ofono.SimManager'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
    " <method name='ChangePin'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
    " <method name='EnterPin'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
    " <method name='ResetPin'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
    " <method name='LockPin'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
    " <method name='UnlockPin'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    "</interface>"
    "<interface name='org.ofono.NetworkRegistration'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='Register'/>"
    " <method name='GetOperators'><arg type='a(oa{sv})' direction='out'/></method>"
    " <method name='Scan'><arg type='a(oa{sv})' direction='out'/></method>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    "</interface>"
    "<interface name='org.ofono.VoiceCallManager'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='GetCalls'><arg type='a(oa{sv})' direction='out'/></method>"
    " <method name='Dial'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
    " <method name='HangupAll'/>"
    " <method name='SendTones'><arg type='s' direction='in'/></method>"
    " <method name='SwapCalls'/>"
    " <method name='ReleaseAndAnswer'/>"
    " <method name='ReleaseAndSwap'/>"
    " <method name='HoldAndAnswer'/>"
    " <method name='Transfer'/>"
    " <method name='CreateMultiparty'><arg type='ao' direction='out'/></method>"
    " <method name='HangupMultiparty'/>"
    " <signal name='CallAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
    " <signal name='CallRemoved'><arg type='o'/></signal>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    "</interface>"
    "<interface name='org.ofono.VoiceCall'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='Answer'/>"
    " <method name='Hangup'/>"
    " <method name='Deflect'><arg type='s' direction='in'/></method>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    "</interface>"
    "<interface name='org.ofono.MessageManager'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
    " <method name='SendMessage'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
    " <method name='GetMessages'><arg type='a(oa{sv})' direction='out'/></method>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    " <signal name='IncomingMessage'><arg type='s'/><arg type='a{sv}'/></signal>"
    " <signal name='ImmediateMessage'><arg type='s'/><arg type='a{sv}'/></signal>"
    " <signal name='MessageAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
    " <signal name='MessageRemoved'><arg type='o'/></signal>"
    "</interface>"
    "<interface name='org.ofono.Message'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='Cancel'/>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    "</interface>"
    "</node>";

void
ofono_reply_error(GDBusMethodInvocation *inv, const char *err, const char *msg)
{
	g_dbus_method_invocation_return_dbus_error(inv, err, msg);
}

static void
emit(const char *path, const char *iface, const char *signal, GVariant *args)
{
	GError *error = NULL;

	if (bus == NULL) {
		g_variant_unref(g_variant_ref_sink(args));
		return;
	}
	if (!g_dbus_connection_emit_signal(bus, NULL, path, iface, signal, args,
	    &error)) {
		mlog("D-Bus: %s.%s: %s", iface, signal, error->message);
		g_error_free(error);
	}
}

/* --- the modem's interfaces ------------------------------------------- */

static gboolean
sim_ready(void)
{
	return M.sim_present && g_strcmp0(M.pin_required, "none") == 0;
}

/* which of the modem's interfaces are usable now (oFono's atoms) */
static GVariant *
interfaces(void)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
	if (M.have_uim)
		g_variant_builder_add(&b, "s", "org.ofono.SimManager");
	if (M.have_voice)
		g_variant_builder_add(&b, "s", "org.ofono.VoiceCallManager");
	if (M.have_wms && sim_ready())
		g_variant_builder_add(&b, "s", "org.ofono.MessageManager");
	if (M.have_nas && M.online)
		g_variant_builder_add(&b, "s", "org.ofono.NetworkRegistration");
	return g_variant_builder_end(&b);
}

static GVariant *
features(void)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
	if (M.have_uim)
		g_variant_builder_add(&b, "s", "sim");
	if (M.have_wms && sim_ready())
		g_variant_builder_add(&b, "s", "sms");
	if (M.have_nas && M.online)
		g_variant_builder_add(&b, "s", "net");
	return g_variant_builder_end(&b);
}

static GVariant *
str_or_empty(const char *s)
{
	return g_variant_new_string(s != NULL ? s : "");
}

static GVariant *
modem_prop(const char *name)
{
	if (!strcmp(name, "Powered"))
		return g_variant_new_boolean(M.powered);
	if (!strcmp(name, "Online"))
		return g_variant_new_boolean(M.online);
	if (!strcmp(name, "Lockdown") || !strcmp(name, "Emergency"))
		return g_variant_new_boolean(FALSE);
	if (!strcmp(name, "Manufacturer"))
		return M.manufacturer ? g_variant_new_string(M.manufacturer) : NULL;
	if (!strcmp(name, "Model"))
		return M.model ? g_variant_new_string(M.model) : NULL;
	if (!strcmp(name, "Revision"))
		return M.revision ? g_variant_new_string(M.revision) : NULL;
	if (!strcmp(name, "Serial"))
		return M.serial ? g_variant_new_string(M.serial) : NULL;
	if (!strcmp(name, "Interfaces"))
		return interfaces();
	if (!strcmp(name, "Features"))
		return features();
	if (!strcmp(name, "Type"))
		return g_variant_new_string("hardware");
	if (!strcmp(name, "Name"))
		return g_variant_new_string("Pro1X");
	return NULL;
}

static const char *const modem_props[] = {
	"Powered", "Online", "Lockdown", "Emergency", "Name", "Manufacturer",
	"Model", "Revision", "Serial", "Features", "Interfaces", "Type", NULL
};

static GVariant *
dict_of(const char *const *names, GVariant *(*get)(const char *))
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	for (; *names != NULL; names++) {
		GVariant *v = get(*names);

		if (v != NULL)
			g_variant_builder_add(&b, "{sv}", *names, v);
	}
	return g_variant_builder_end(&b);
}

static void
prop_changed(const char *path, const char *iface, const char *name,
    GVariant *(*get)(const char *))
{
	GVariant *v = get(name);

	if (v == NULL)
		return;
	emit(path, iface, "PropertyChanged", g_variant_new("(sv)", name, v));
}

void
ofono_modem_changed(const char *prop)
{
	prop_changed(M.path, "org.ofono.Modem", prop, modem_prop);
}

void
ofono_interfaces_changed(void)
{
	static char *last;
	GVariant *v = g_variant_ref_sink(interfaces());
	char *s = g_variant_print(v, FALSE);

	if (g_strcmp0(s, last) != 0) {
		mlog("interfaces: %s", s);
		g_free(last);
		last = s;
		ofono_modem_changed("Interfaces");
		ofono_modem_changed("Features");
	} else {
		g_free(s);
	}
	g_variant_unref(v);
}

/* --- SimManager ------------------------------------------------------- */

static GVariant *
sim_prop(const char *name)
{
	if (!strcmp(name, "Present"))
		return g_variant_new_boolean(M.sim_present);
	if (!M.sim_present)
		return NULL;
	if (!strcmp(name, "PinRequired"))
		return str_or_empty(M.pin_required ? M.pin_required : "none");
	if (!strcmp(name, "SubscriberIdentity"))
		return M.imsi ? g_variant_new_string(M.imsi) : NULL;
	if (!strcmp(name, "CardIdentifier"))
		return M.iccid ? g_variant_new_string(M.iccid) : NULL;
	if (!strcmp(name, "FixedDialing") || !strcmp(name, "BarredDialing"))
		return g_variant_new_boolean(FALSE);
	if (!strcmp(name, "SubscriberNumbers"))
		return g_variant_new_strv(NULL, 0);
	if (!strcmp(name, "LockedPins")) {
		const char *p[] = { "pin", NULL };
		gboolean locked = g_strcmp0(M.pin_required, "pin") == 0 ||
		    g_strcmp0(M.pin_required, "puk") == 0;

		return g_variant_new_strv(locked ? p : NULL, locked ? 1 : 0);
	}
	if (!strcmp(name, "Retries")) {
		GVariantBuilder b;

		g_variant_builder_init(&b, G_VARIANT_TYPE("a{sy}"));
		if (M.pin_retries >= 0)
			g_variant_builder_add(&b, "{sy}", "pin", (guchar)M.pin_retries);
		if (M.puk_retries >= 0)
			g_variant_builder_add(&b, "{sy}", "puk", (guchar)M.puk_retries);
		return g_variant_builder_end(&b);
	}
	return NULL;
}

static const char *const sim_props[] = {
	"Present", "SubscriberIdentity", "SubscriberNumbers", "PinRequired",
	"LockedPins", "CardIdentifier", "FixedDialing", "BarredDialing",
	"Retries", NULL
};

void
ofono_sim_changed(const char *prop)
{
	prop_changed(M.path, "org.ofono.SimManager", prop, sim_prop);
}

/* --- NetworkRegistration ---------------------------------------------- */

static GVariant *
netreg_prop(const char *name)
{
	if (!strcmp(name, "Mode"))
		return g_variant_new_string("auto");
	if (!strcmp(name, "Status"))
		return str_or_empty(M.reg_status ? M.reg_status : "unknown");
	if (!strcmp(name, "Name"))
		return str_or_empty(M.op_name);
	if (!strcmp(name, "LocationAreaCode"))
		return M.have_lac ? g_variant_new_uint16(M.lac) : NULL;
	if (!strcmp(name, "CellId"))
		return M.have_cell ? g_variant_new_uint32(M.cell_id) : NULL;
	if (!strcmp(name, "MobileCountryCode"))
		return M.op_mcc ? g_variant_new_string(M.op_mcc) : NULL;
	if (!strcmp(name, "MobileNetworkCode"))
		return M.op_mnc ? g_variant_new_string(M.op_mnc) : NULL;
	if (!strcmp(name, "Technology"))
		return M.technology ? g_variant_new_string(M.technology) : NULL;
	if (!strcmp(name, "Strength"))
		return M.strength >= 0 ? g_variant_new_byte((guchar)M.strength) : NULL;
	return NULL;
}

static const char *const netreg_props[] = {
	"Mode", "Status", "LocationAreaCode", "CellId", "MobileCountryCode",
	"MobileNetworkCode", "Technology", "Name", "Strength", NULL
};

void
ofono_netreg_changed(const char *prop)
{
	prop_changed(M.path, "org.ofono.NetworkRegistration", prop, netreg_prop);
}

/* --- calls ------------------------------------------------------------ */

static struct call *prop_call;

static GVariant *
call_prop(const char *name)
{
	struct call *c = prop_call;

	if (!strcmp(name, "LineIdentification"))
		return str_or_empty(c->number);
	if (!strcmp(name, "Name"))
		return g_variant_new_string("");
	if (!strcmp(name, "State"))
		return g_variant_new_string(call_state_name(c->state));
	if (!strcmp(name, "Multiparty") || !strcmp(name, "RemoteHeld") ||
	    !strcmp(name, "RemoteMultiparty"))
		return g_variant_new_boolean(FALSE);
	if (!strcmp(name, "Emergency"))
		return g_variant_new_boolean(c->emergency);
	if (!strcmp(name, "StartTime"))
		return c->start_time[0] ? g_variant_new_string(c->start_time) : NULL;
	return NULL;
}

static const char *const call_props[] = {
	"LineIdentification", "Name", "Multiparty", "State", "StartTime",
	"Emergency", "RemoteHeld", "RemoteMultiparty", NULL
};

static GVariant *
call_dict(struct call *c)
{
	prop_call = c;
	return dict_of(call_props, call_prop);
}

static void call_method(GDBusConnection *, const char *, const char *,
    const char *, const char *, GVariant *, GDBusMethodInvocation *, gpointer);
static const GDBusInterfaceVTable call_vtable = { call_method, NULL, NULL, { 0 } };

void
ofono_call_added(struct call *c)
{
	GError *error = NULL;

	c->reg = g_dbus_connection_register_object(bus, c->path,
	    g_dbus_node_info_lookup_interface(node_info, "org.ofono.VoiceCall"),
	    &call_vtable, NULL, NULL, &error);
	if (c->reg == 0) {
		mlog("D-Bus: %s: %s", c->path, error->message);
		g_error_free(error);
	}
	emit(M.path, "org.ofono.VoiceCallManager", "CallAdded",
	    g_variant_new("(o@a{sv})", c->path, call_dict(c)));
}

void
ofono_call_changed(struct call *c, const char *prop)
{
	prop_call = c;
	prop_changed(c->path, "org.ofono.VoiceCall", prop, call_prop);
}

void
ofono_call_removed(struct call *c)
{
	emit(M.path, "org.ofono.VoiceCallManager", "CallRemoved",
	    g_variant_new("(o)", c->path));
	if (c->reg != 0)
		g_dbus_connection_unregister_object(bus, c->reg);
	c->reg = 0;
}

static void
call_method(GDBusConnection *conn, const char *sender, const char *path,
    const char *iface, const char *method, GVariant *args,
    GDBusMethodInvocation *inv, gpointer data)
{
	struct call *c = call_find_path(path);

	(void)conn; (void)sender; (void)iface; (void)args; (void)data;
	if (c == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_FAILED, "no such call");
		return;
	}
	if (!strcmp(method, "GetProperties"))
		g_dbus_method_invocation_return_value(inv,
		    g_variant_new("(@a{sv})", call_dict(c)));
	else if (!strcmp(method, "Answer"))
		qmi_answer(c, inv);
	else if (!strcmp(method, "Hangup"))
		qmi_hangup(c, inv);
	else
		ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
}

static GVariant *
calls_array(void)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a(oa{sv})"));
	for (GList *l = M.calls; l != NULL; l = l->next) {
		struct call *c = l->data;

		if (c->state != CALL_DISCONNECTED)
			g_variant_builder_add(&b, "(o@a{sv})", c->path, call_dict(c));
	}
	return g_variant_builder_end(&b);
}

/* --- messages --------------------------------------------------------- */

static struct message *prop_msg;
static guint msg_counter;

static GVariant *
msg_prop(const char *name)
{
	if (!strcmp(name, "State"))
		return g_variant_new_string(prop_msg->state);
	return NULL;
}

static const char *const msg_props[] = { "State", NULL };

static GVariant *
msg_dict(struct message *m)
{
	prop_msg = m;
	return dict_of(msg_props, msg_prop);
}

static void
msg_method(GDBusConnection *conn, const char *sender, const char *path,
    const char *iface, const char *method, GVariant *args,
    GDBusMethodInvocation *inv, gpointer data)
{
	(void)conn; (void)sender; (void)iface; (void)args; (void)data;
	for (GList *l = M.messages; l != NULL; l = l->next) {
		struct message *m = l->data;

		if (strcmp(m->path, path) != 0)
			continue;
		if (!strcmp(method, "GetProperties"))
			g_dbus_method_invocation_return_value(inv,
			    g_variant_new("(@a{sv})", msg_dict(m)));
		else	/* Cancel: the parts are with the modem at once */
			ofono_reply_error(inv, "org.ofono.Error.AccessDenied",
			    "the message is with the modem");
		return;
	}
	ofono_reply_error(inv, OFONO_ERROR_FAILED, "no such message");
}

static const GDBusInterfaceVTable msg_vtable = { msg_method, NULL, NULL, { 0 } };

struct message *
ofono_message_new(int parts)
{
	struct message *m = g_new0(struct message, 1);
	GError *error = NULL;

	m->n = ++msg_counter;
	m->path = g_strdup_printf("%s/message_%02u", M.path, m->n);
	m->state = "pending";
	m->parts = parts;
	M.messages = g_list_append(M.messages, m);
	m->reg = g_dbus_connection_register_object(bus, m->path,
	    g_dbus_node_info_lookup_interface(node_info, "org.ofono.Message"),
	    &msg_vtable, NULL, NULL, &error);
	if (m->reg == 0) {
		mlog("D-Bus: %s: %s", m->path, error->message);
		g_error_free(error);
	}
	emit(M.path, "org.ofono.MessageManager", "MessageAdded",
	    g_variant_new("(o@a{sv})", m->path, msg_dict(m)));
	return m;
}

/* a final state removes the object, as oFono does */
void
ofono_message_state(struct message *m, const char *state)
{
	if (g_strcmp0(m->state, state) == 0)
		return;
	m->state = state;
	prop_msg = m;
	prop_changed(m->path, "org.ofono.Message", "State", msg_prop);
	if (!strcmp(state, "sent") || !strcmp(state, "failed")) {
		emit(M.path, "org.ofono.MessageManager", "MessageRemoved",
		    g_variant_new("(o)", m->path));
		if (m->reg != 0)
			g_dbus_connection_unregister_object(bus, m->reg);
		M.messages = g_list_remove(M.messages, m);
		g_free(m->path);
		g_free(m);
	}
}

/*
 * IncomingMessage (ImmediateMessage for class 0) with Sender,
 * LocalSentTime (the sender's local time and zone, which sphone reads with
 * strptime("%Y-%m-%dT%H:%M:%S%z")) and SentTime (oFono: the same in UTC).
 */
void
ofono_incoming_message(const char *text, const char *sender, time_t t,
    int tz_minutes, gboolean immediate)
{
	GVariantBuilder b;
	char local[40], utc[40];
	time_t lt;
	struct tm tm;

	if (t == 0)
		t = time(NULL);
	lt = t + tz_minutes * 60;
	gmtime_r(&lt, &tm);
	strftime(local, sizeof(local) - 12, "%Y-%m-%dT%H:%M:%S", &tm);
	snprintf(local + strlen(local), 12, "%c%02d%02d", tz_minutes < 0 ? '-' : '+',
	    (abs(tz_minutes) / 60) % 100, abs(tz_minutes) % 60);
	gmtime_r(&t, &tm);
	strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%S+0000", &tm);
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Sender", g_variant_new_string(sender));
	g_variant_builder_add(&b, "{sv}", "LocalSentTime", g_variant_new_string(local));
	g_variant_builder_add(&b, "{sv}", "SentTime", g_variant_new_string(utc));
	mlog("sms: from %s, %s, %zu bytes%s", sender, local, strlen(text),
	    immediate ? ", class 0" : "");
	emit(M.path, "org.ofono.MessageManager",
	    immediate ? "ImmediateMessage" : "IncomingMessage",
	    g_variant_new("(sa{sv})", text, &b));
}

static GVariant *
mm_prop(const char *name)
{
	if (!strcmp(name, "ServiceCenterAddress"))
		return g_variant_new_string("");
	if (!strcmp(name, "UseDeliveryReports"))
		return g_variant_new_boolean(FALSE);
	if (!strcmp(name, "Bearer"))
		return g_variant_new_string("cs-preferred");
	if (!strcmp(name, "Alphabet"))
		return g_variant_new_string("default");
	return NULL;
}

static const char *const mm_props[] = {
	"ServiceCenterAddress", "UseDeliveryReports", "Bearer", "Alphabet", NULL
};

/* --- the modem object's methods --------------------------------------- */

static GVariant *
vcm_prop(const char *name)
{
	/* 22.101's numbers, as oFono's default list */
	static const char *const nums[] = { "08", "000", "999", "110", "112", "911",
	    "118", "119", NULL };

	if (!strcmp(name, "EmergencyNumbers"))
		return g_variant_new_strv(nums, -1);
	return NULL;
}

static const char *const vcm_props[] = { "EmergencyNumbers", NULL };

static void
modem_method(GDBusConnection *conn, const char *sender, const char *path,
    const char *iface, const char *method, GVariant *args,
    GDBusMethodInvocation *inv, gpointer data)
{
	(void)conn; (void)sender; (void)path; (void)data;
	mdbg("D-Bus: %s.%s from %s", iface, method, sender);
	if (!strcmp(method, "GetProperties")) {
		GVariant *d;

		if (!strcmp(iface, "org.ofono.Modem"))
			d = dict_of(modem_props, modem_prop);
		else if (!strcmp(iface, "org.ofono.SimManager"))
			d = dict_of(sim_props, sim_prop);
		else if (!strcmp(iface, "org.ofono.NetworkRegistration"))
			d = dict_of(netreg_props, netreg_prop);
		else if (!strcmp(iface, "org.ofono.VoiceCallManager"))
			d = dict_of(vcm_props, vcm_prop);
		else
			d = dict_of(mm_props, mm_prop);
		g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", d));
		return;
	}
	if (!strcmp(iface, "org.ofono.Modem") && !strcmp(method, "SetProperty")) {
		const char *name;
		GVariant *v;

		g_variant_get(args, "(&sv)", &name, &v);
		if ((!strcmp(name, "Online") || !strcmp(name, "Powered")) &&
		    g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
			gboolean on = g_variant_get_boolean(v);

			/* Powered: the modem's DSP is pro1x-dsp's; off is
			 * low power, as Online off */
			if (!strcmp(name, "Powered") && on)
				g_dbus_method_invocation_return_value(inv, NULL);
			else
				qmi_set_online(on, inv);
		} else {
			ofono_reply_error(inv, OFONO_ERROR_INVALID_ARGS, name);
		}
		g_variant_unref(v);
		return;
	}
	if (!strcmp(iface, "org.ofono.SimManager")) {
		const char *a, *b, *c;

		if (!strcmp(method, "EnterPin")) {
			g_variant_get(args, "(&s&s)", &a, &b);
			qmi_enter_pin(a, b, inv);
		} else if (!strcmp(method, "ResetPin")) {
			g_variant_get(args, "(&s&s&s)", &a, &b, &c);
			qmi_reset_pin(a, b, c, inv);
		} else {
			ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
		}
		return;
	}
	if (!strcmp(iface, "org.ofono.NetworkRegistration")) {
		if (!strcmp(method, "Register"))
			qmi_register(inv);
		else if (!strcmp(method, "GetOperators"))
			g_dbus_method_invocation_return_value(inv,
			    g_variant_new("(@a(oa{sv}))",
			    g_variant_new_array(G_VARIANT_TYPE("(oa{sv})"), NULL, 0)));
		else
			ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
		return;
	}
	if (!strcmp(iface, "org.ofono.VoiceCallManager")) {
		const char *a, *b;

		if (!strcmp(method, "GetCalls")) {
			g_dbus_method_invocation_return_value(inv,
			    g_variant_new("(@a(oa{sv}))", calls_array()));
		} else if (!strcmp(method, "Dial")) {
			g_variant_get(args, "(&s&s)", &a, &b);
			qmi_dial(a, b, inv);
		} else if (!strcmp(method, "HangupAll")) {
			qmi_hangup_all(inv);
		} else if (!strcmp(method, "SendTones")) {
			g_variant_get(args, "(&s)", &a);
			qmi_send_tones(a, inv);
		} else {
			ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
		}
		return;
	}
	if (!strcmp(iface, "org.ofono.MessageManager")) {
		const char *a, *b;

		if (!strcmp(method, "SendMessage")) {
			g_variant_get(args, "(&s&s)", &a, &b);
			qmi_send_sms(a, b, inv);
		} else if (!strcmp(method, "GetMessages")) {
			GVariantBuilder bl;

			g_variant_builder_init(&bl, G_VARIANT_TYPE("a(oa{sv})"));
			for (GList *l = M.messages; l != NULL; l = l->next) {
				struct message *m = l->data;

				g_variant_builder_add(&bl, "(o@a{sv})", m->path, msg_dict(m));
			}
			g_dbus_method_invocation_return_value(inv,
			    g_variant_new("(a(oa{sv}))", &bl));
		} else {
			ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
		}
		return;
	}
	ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
}

static const GDBusInterfaceVTable modem_vtable = { modem_method, NULL, NULL, { 0 } };

static void
manager_method(GDBusConnection *conn, const char *sender, const char *path,
    const char *iface, const char *method, GVariant *args,
    GDBusMethodInvocation *inv, gpointer data)
{
	GVariantBuilder b;

	(void)conn; (void)sender; (void)path; (void)iface; (void)args; (void)data;
	mdbg("D-Bus: Manager.%s from %s", method, sender);
	g_variant_builder_init(&b, G_VARIANT_TYPE("a(oa{sv})"));
	g_variant_builder_add(&b, "(o@a{sv})", M.path, dict_of(modem_props, modem_prop));
	g_dbus_method_invocation_return_value(inv, g_variant_new("(a(oa{sv}))", &b));
}

static const GDBusInterfaceVTable manager_vtable = { manager_method, NULL, NULL, { 0 } };

static void
name_acquired(GDBusConnection *conn, const char *name, gpointer data)
{
	(void)conn; (void)data;
	mlog("D-Bus: %s acquired, modem at %s", name, M.path);
	emit("/", "org.ofono.Manager", "ModemAdded",
	    g_variant_new("(o@a{sv})", M.path, dict_of(modem_props, modem_prop)));
}

static void
name_lost(GDBusConnection *conn, const char *name, gpointer data)
{
	(void)conn; (void)data;
	mlog("D-Bus: %s not ours (another oFono running, or no policy for it)", name);
	modemd_quit(1);
}

int
ofono_init(GDBusConnection *conn)
{
	static const char *const ifaces[] = {
		"org.ofono.Modem", "org.ofono.SimManager",
		"org.ofono.NetworkRegistration", "org.ofono.VoiceCallManager",
		"org.ofono.MessageManager", NULL
	};
	GError *error = NULL;

	bus = conn;
	node_info = g_dbus_node_info_new_for_xml(introspection, &error);
	g_assert_no_error(error);
	if (g_dbus_connection_register_object(bus, "/",
	    g_dbus_node_info_lookup_interface(node_info, "org.ofono.Manager"),
	    &manager_vtable, NULL, NULL, &error) == 0)
		goto fail;
	for (int i = 0; ifaces[i] != NULL; i++)
		if (g_dbus_connection_register_object(bus, M.path,
		    g_dbus_node_info_lookup_interface(node_info, ifaces[i]),
		    &modem_vtable, NULL, NULL, &error) == 0)
			goto fail;
	g_bus_own_name_on_connection(bus, OFONO_SERVICE,
	    G_BUS_NAME_OWNER_FLAGS_NONE, name_acquired, name_lost, NULL, NULL);
	return 0;
fail:
	mlog("D-Bus: %s", error->message);
	g_error_free(error);
	return -1;
}
