/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Mobile data (freebsd/data/README.md, stage 4): the modem's data port set
 * up as ModemManager 1.24's qcom-soc support and Leste's patched oFono
 * (patches/fixes/ofono.patch, docs/leste-mobile-data.md) do it for the IPA,
 * data sessions with WDS, and oFono's ConnectionManager and
 * ConnectionContext on D-Bus (ofono 2.18's doc/connman-api.txt, its names
 * and values). Every QMI message is libqmi's.
 *
 *  1. DPM "Open Port": the IPA's endpoints facing the modem bound to the
 *     embedded data port (EMBEDDED, interface 1); the modem's RX endpoint
 *     is the AP's TX one (qcom_ipa's dev.qcom_ipa.0.modem.tx_endpoint_id,
 *     Linux's sysfs modem/tx_endpoint_id), and the other way round.
 *  2. WDA "Set Data Format" on that port: raw IP, QMAPv5, else QMAPv4,
 *     else QMAP (32 datagrams, 32768 bytes downlink), each read back with
 *     "Get Data Format"; the endpoint info only once the modem asks for it
 *     (MISSING_ARGUMENT).
 *  3. A context's activation, per IP family on its own WDS client: "Bind
 *     Mux Data Port" (EMBEDDED, 1, the context's mux id), "Set IP Family",
 *     "Start Network" (APN, authentication), "Get Current Settings"; the
 *     address goes onto the QMAP interface of that mux id (qmapN for mux
 *     N + 1) through the hook (-I, pro1x-data-ifup: ifconfig, route,
 *     resolvconf). Raw IP over QMAP has no DHCP: oFono's Method "static".
 *  4. "Packet Service Status" indications end a session; "Stop Network"
 *     ends it on request.
 *
 * The APN and the rest come from the user's file (-c, the key=value lines
 * of data.conf: apn, username, password, auth, protocol, roaming,
 * autoconnect) and from SetProperty; nothing is written back. The password
 * is never logged.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

#include "modemd.h"

#define	NCTX		4
#define	ALLOC_TRIES	24		/* every 5 s: two minutes */
#define	QMI_TIMEOUT	10
#define	START_TIMEOUT	60		/* Start Network may take a while */
#define	DL_DATAGRAMS	32
#define	DL_SIZE		32768

struct data_options dopt = {
	.conf = "/dev/pro1x-host/freebsd/data.conf",
	.hook = "/usr/local/libexec/pro1x-data-ifup",
	.ifprefix = "qmap",
	.ipa_sysctl = "dev.qcom_ipa.0",
};

enum dp_state { DP_NONE, DP_SETUP, DP_READY, DP_FAILED };

struct family {
	QmiClientWds	*wds;
	gboolean	bound, up;
	guint32		handle;
	guint		ind;		/* packet service status handler */
	char		addr[INET6_ADDRSTRLEN], gw[INET6_ADDRSTRLEN];
	char		mask[INET_ADDRSTRLEN];
	guint8		prefix;
	char		dns[2][INET6_ADDRSTRLEN];
	guint32		mtu;
	char		reason[160];
};

struct context {
	guint		n;
	char		*path;
	char		*name, *type, *apn, *user, *pass, *auth, *proto;
	gboolean	active;
	guint8		mux;
	char		ifname[IFNAMSIZ];
	guint		reg;
	struct family	f4, f6;
	/* an activation or deactivation under way, its caller */
	GDBusMethodInvocation *inv;
	gboolean	activating, deactivating;
	int		step_family;	/* 4 or 6 */
};

static GDBusConnection *bus;
static GDBusNodeInfo *node_info;
static QmiDevice *dev;
static QmiClientWda *wda;
static QmiClientDpm *dpm;
static QmiClientWds *wds_spare;		/* the first WDS client, for IPv4 */
static enum dp_state dp;
static guint alloc_tries[3];
static int wda_try, wda_end;
static gboolean wda_endpoint;
static struct context *ctxs[NCTX];
static gboolean cm_powered = TRUE, cm_roaming;
static gboolean last_attached;
static const char *last_bearer;
static gboolean autoconnect;

static const guint32 wda_protocols[] = {
	QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV5,
	QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV4,
	QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAP,
};
static const char *const wda_names[] = { "qmapv5", "qmapv4", "qmap" };

static const char introspection[] =
    "<node>"
    "<interface name='org.ofono.ConnectionManager'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
    " <method name='AddContext'><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
    " <method name='RemoveContext'><arg type='o' direction='in'/></method>"
    " <method name='DeactivateAll'/>"
    " <method name='GetContexts'><arg type='a(oa{sv})' direction='out'/></method>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    " <signal name='ContextAdded'><arg type='o'/><arg type='a{sv}'/></signal>"
    " <signal name='ContextRemoved'><arg type='o'/></signal>"
    "</interface>"
    "<interface name='org.ofono.ConnectionContext'>"
    " <method name='GetProperties'><arg type='a{sv}' direction='out'/></method>"
    " <method name='SetProperty'><arg type='s' direction='in'/><arg type='v' direction='in'/></method>"
    " <signal name='PropertyChanged'><arg type='s'/><arg type='v'/></signal>"
    "</interface>"
    "</node>";

static void ctx_activate_next(struct context *c);
static void ctx_down(struct context *c, const char *why);
static void maybe_autoconnect(void);

/* --- small things --------------------------------------------------------- */

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

static gboolean
sim_ready(void)
{
	return M.sim_present && g_strcmp0(M.pin_required, "none") == 0;
}

/* ConnectionManager's Attached: registered (NAS's state, not PS attach) */
static gboolean
attached(void)
{
	return !g_strcmp0(M.reg_status, "registered") ||
	    !g_strcmp0(M.reg_status, "roaming");
}

/* oFono's Bearer from the radio technology */
static const char *
bearer(void)
{
	if (!attached() || M.technology == NULL)
		return "none";
	if (!strcmp(M.technology, "lte") || !strcmp(M.technology, "nr"))
		return "lte";
	if (!strcmp(M.technology, "umts"))
		return "umts";
	if (!strcmp(M.technology, "gsm"))
		return "gprs";
	return "none";
}

gboolean
data_available(void)
{
	return dp == DP_READY;
}

static const char *
qmi_err(GError *e)
{
	return e != NULL ? e->message : "?";
}

static gboolean
proto_has(const struct context *c, int family)
{
	if (family == 4)
		return strcmp(c->proto, "ipv6") != 0;
	return strcmp(c->proto, "ipv6") == 0 || strcmp(c->proto, "dual") == 0;
}

/* --- the hook: the addresses onto the interface ------------------------------ */

static void
run_hook(struct context *c, gboolean up)
{
	char *argv[] = { (char *)dopt.hook, up ? "up" : "down", c->ifname, NULL };
	char **env = g_get_environ();
	GError *error = NULL;
	gchar *out = NULL;
	gint status = 0;
	char b[16];

	if (dopt.hook == NULL || dopt.hook[0] == '\0')
		return;
	env = g_environ_setenv(env, "INTERFACE", c->ifname, TRUE);
	if (up && c->f4.up) {
		env = g_environ_setenv(env, "IPV4_ADDRESS", c->f4.addr, TRUE);
		env = g_environ_setenv(env, "IPV4_NETMASK", c->f4.mask, TRUE);
		env = g_environ_setenv(env, "IPV4_GATEWAY", c->f4.gw, TRUE);
		env = g_environ_setenv(env, "IPV4_DNS1", c->f4.dns[0], TRUE);
		env = g_environ_setenv(env, "IPV4_DNS2", c->f4.dns[1], TRUE);
	}
	if (up && c->f6.up) {
		env = g_environ_setenv(env, "IPV6_ADDRESS", c->f6.addr, TRUE);
		snprintf(b, sizeof(b), "%u", c->f6.prefix);
		env = g_environ_setenv(env, "IPV6_PREFIXLEN", b, TRUE);
		env = g_environ_setenv(env, "IPV6_GATEWAY", c->f6.gw, TRUE);
		env = g_environ_setenv(env, "IPV6_DNS1", c->f6.dns[0], TRUE);
		env = g_environ_setenv(env, "IPV6_DNS2", c->f6.dns[1], TRUE);
	}
	if (up) {
		guint32 mtu = c->f4.up && c->f4.mtu ? c->f4.mtu : c->f6.mtu;

		if (mtu != 0) {
			snprintf(b, sizeof(b), "%u", mtu);
			env = g_environ_setenv(env, "MTU", b, TRUE);
		}
	}
	if (!g_spawn_sync(NULL, argv, env, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
	    &out, NULL, &status, &error)) {
		mlog("data: %s: %s", dopt.hook, error->message);
		g_error_free(error);
	} else {
		if (out != NULL)
			for (char *l = strtok(out, "\n"); l != NULL; l = strtok(NULL, "\n"))
				mlog("data: %s", l);
		mlog("data: %s %s %s: %s", dopt.hook, up ? "up" : "down", c->ifname,
		    status == 0 ? "done" : "failed");
	}
	g_free(out);
	g_strfreev(env);
}

/* --- D-Bus: properties --------------------------------------------------------- */

static GVariant *
settings4(const struct context *c)
{
	GVariantBuilder b, d;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	if (c->active && c->f4.up) {
		g_variant_builder_add(&b, "{sv}", "Interface", g_variant_new_string(c->ifname));
		g_variant_builder_add(&b, "{sv}", "Method", g_variant_new_string("static"));
		g_variant_builder_add(&b, "{sv}", "Address", g_variant_new_string(c->f4.addr));
		g_variant_builder_add(&b, "{sv}", "Netmask", g_variant_new_string(c->f4.mask));
		if (c->f4.gw[0])
			g_variant_builder_add(&b, "{sv}", "Gateway",
			    g_variant_new_string(c->f4.gw));
		g_variant_builder_init(&d, G_VARIANT_TYPE("as"));
		for (int i = 0; i < 2; i++)
			if (c->f4.dns[i][0])
				g_variant_builder_add(&d, "s", c->f4.dns[i]);
		g_variant_builder_add(&b, "{sv}", "DomainNameServers",
		    g_variant_builder_end(&d));
	}
	return g_variant_builder_end(&b);
}

static GVariant *
settings6(const struct context *c)
{
	GVariantBuilder b, d;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	if (c->active && c->f6.up) {
		g_variant_builder_add(&b, "{sv}", "Interface", g_variant_new_string(c->ifname));
		g_variant_builder_add(&b, "{sv}", "Address", g_variant_new_string(c->f6.addr));
		g_variant_builder_add(&b, "{sv}", "PrefixLength", g_variant_new_byte(c->f6.prefix));
		if (c->f6.gw[0])
			g_variant_builder_add(&b, "{sv}", "Gateway",
			    g_variant_new_string(c->f6.gw));
		g_variant_builder_init(&d, G_VARIANT_TYPE("as"));
		for (int i = 0; i < 2; i++)
			if (c->f6.dns[i][0])
				g_variant_builder_add(&d, "s", c->f6.dns[i]);
		g_variant_builder_add(&b, "{sv}", "DomainNameServers",
		    g_variant_builder_end(&d));
	}
	return g_variant_builder_end(&b);
}

static GVariant *
ctx_dict(const struct context *c)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Active", g_variant_new_boolean(c->active));
	g_variant_builder_add(&b, "{sv}", "AccessPointName", g_variant_new_string(c->apn));
	g_variant_builder_add(&b, "{sv}", "Type", g_variant_new_string(c->type));
	g_variant_builder_add(&b, "{sv}", "AuthenticationMethod", g_variant_new_string(c->auth));
	g_variant_builder_add(&b, "{sv}", "Username", g_variant_new_string(c->user));
	g_variant_builder_add(&b, "{sv}", "Password", g_variant_new_string(c->pass));
	g_variant_builder_add(&b, "{sv}", "Protocol", g_variant_new_string(c->proto));
	g_variant_builder_add(&b, "{sv}", "Name", g_variant_new_string(c->name));
	g_variant_builder_add(&b, "{sv}", "Settings", settings4(c));
	g_variant_builder_add(&b, "{sv}", "IPv6.Settings", settings6(c));
	return g_variant_builder_end(&b);
}

static void
ctx_changed(struct context *c, const char *prop, GVariant *v)
{
	emit(c->path, "org.ofono.ConnectionContext", "PropertyChanged",
	    g_variant_new("(sv)", prop, v));
}

static void
ctx_active_changed(struct context *c)
{
	ctx_changed(c, "Settings", settings4(c));
	ctx_changed(c, "IPv6.Settings", settings6(c));
	ctx_changed(c, "Active", g_variant_new_boolean(c->active));
}

static GVariant *
cm_dict(void)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Attached", g_variant_new_boolean(attached()));
	g_variant_builder_add(&b, "{sv}", "Bearer", g_variant_new_string(bearer()));
	g_variant_builder_add(&b, "{sv}", "Suspended", g_variant_new_boolean(FALSE));
	g_variant_builder_add(&b, "{sv}", "RoamingAllowed", g_variant_new_boolean(cm_roaming));
	g_variant_builder_add(&b, "{sv}", "Powered", g_variant_new_boolean(cm_powered));
	return g_variant_builder_end(&b);
}

static void
cm_changed(const char *prop, GVariant *v)
{
	emit(M.path, "org.ofono.ConnectionManager", "PropertyChanged",
	    g_variant_new("(sv)", prop, v));
}

/* the registration changed (ofono.c's ofono_netreg_changed) */
void
data_netreg_changed(void)
{
	gboolean a = attached();
	const char *b = bearer();

	if (a != last_attached) {
		last_attached = a;
		cm_changed("Attached", g_variant_new_boolean(a));
	}
	if (last_bearer == NULL || strcmp(b, last_bearer) != 0) {
		last_bearer = b;
		cm_changed("Bearer", g_variant_new_string(b));
	}
	maybe_autoconnect();
}

/* --- a context's session ---------------------------------------------------------- */

static void
ctx_reply(struct context *c, const char *err, const char *msg)
{
	if (c->inv == NULL)
		return;
	if (err == NULL)
		g_dbus_method_invocation_return_value(c->inv, NULL);
	else
		ofono_reply_error(c->inv, err, msg);
	c->inv = NULL;
}

static void
fam_clear(struct family *f)
{
	f->up = FALSE;
	f->handle = 0;
	f->addr[0] = f->gw[0] = f->mask[0] = '\0';
	f->dns[0][0] = f->dns[1][0] = '\0';
	f->prefix = 0;
	f->mtu = 0;
}

/* all families tried: the context is up when one of them is */
static void
ctx_activation_done(struct context *c)
{
	c->activating = FALSE;
	if (!c->f4.up && !c->f6.up) {
		char msg[400];

		snprintf(msg, sizeof(msg), "no session%s%s%s%s",
		    c->f4.reason[0] ? ": IPv4 " : "", c->f4.reason,
		    c->f6.reason[0] ? ", IPv6 " : "", c->f6.reason);
		mlog("data: context %u: %s", c->n, msg);
		ctx_reply(c, OFONO_ERROR_FAILED, msg);
		return;
	}
	c->active = TRUE;
	run_hook(c, TRUE);
	mlog("data: context %u active on %s%s%s%s%s", c->n, c->ifname,
	    c->f4.up ? ", " : "", c->f4.up ? c->f4.addr : "",
	    c->f6.up ? ", " : "", c->f6.up ? c->f6.addr : "");
	ctx_active_changed(c);
	ctx_reply(c, NULL, NULL);
}

static void
ip4_str(guint32 a, char *buf, size_t len)
{
	struct in_addr in;

	in.s_addr = htonl(a);
	inet_ntop(AF_INET, &in, buf, len);
}

static void
ip6_str(GArray *a, char *buf, size_t len)
{
	struct in6_addr in;

	if (a == NULL || a->len != 8) {
		buf[0] = '\0';
		return;
	}
	for (guint i = 0; i < 8; i++) {
		guint16 w = g_array_index(a, guint16, i);

		in.s6_addr[2 * i] = w >> 8;
		in.s6_addr[2 * i + 1] = w & 0xff;
	}
	inet_ntop(AF_INET6, &in, buf, len);
}

static void
settings_ready(QmiClientWds *wds, GAsyncResult *res, struct context *c)
{
	struct family *f = c->step_family == 4 ? &c->f4 : &c->f6;
	QmiMessageWdsGetCurrentSettingsOutput *o;
	GError *error = NULL;
	guint32 v4, mask, mtu;
	GArray *a6;
	guint8 plen;

	o = qmi_client_wds_get_current_settings_finish(wds, res, &error);
	if (o == NULL || !qmi_message_wds_get_current_settings_output_get_result(o,
	    &error)) {
		/* the session is up, its settings not known: nothing to put on */
		mlog("data: context %u IPv%d: Get Current Settings: %s", c->n,
		    c->step_family, qmi_err(error));
		snprintf(f->reason, sizeof(f->reason), "no settings");
		f->up = FALSE;
		g_clear_error(&error);
	} else if (c->step_family == 4) {
		if (qmi_message_wds_get_current_settings_output_get_ipv4_address(o, &v4,
		    NULL)) {
			ip4_str(v4, f->addr, sizeof(f->addr));
			mask = 0xffffffffU;
			qmi_message_wds_get_current_settings_output_get_ipv4_gateway_subnet_mask(
			    o, &mask, NULL);
			ip4_str(mask, f->mask, sizeof(f->mask));
			if (qmi_message_wds_get_current_settings_output_get_ipv4_gateway_address(
			    o, &v4, NULL))
				ip4_str(v4, f->gw, sizeof(f->gw));
			if (qmi_message_wds_get_current_settings_output_get_primary_ipv4_dns_address(
			    o, &v4, NULL))
				ip4_str(v4, f->dns[0], sizeof(f->dns[0]));
			if (qmi_message_wds_get_current_settings_output_get_secondary_ipv4_dns_address(
			    o, &v4, NULL))
				ip4_str(v4, f->dns[1], sizeof(f->dns[1]));
		} else {
			snprintf(f->reason, sizeof(f->reason), "no IPv4 address");
			f->up = FALSE;
		}
	} else {
		if (qmi_message_wds_get_current_settings_output_get_ipv6_address(o, &a6,
		    &plen, NULL)) {
			ip6_str(a6, f->addr, sizeof(f->addr));
			f->prefix = plen;
			if (qmi_message_wds_get_current_settings_output_get_ipv6_gateway_address(
			    o, &a6, &plen, NULL))
				ip6_str(a6, f->gw, sizeof(f->gw));
			if (qmi_message_wds_get_current_settings_output_get_ipv6_primary_dns_address(
			    o, &a6, NULL))
				ip6_str(a6, f->dns[0], sizeof(f->dns[0]));
			if (qmi_message_wds_get_current_settings_output_get_ipv6_secondary_dns_address(
			    o, &a6, NULL))
				ip6_str(a6, f->dns[1], sizeof(f->dns[1]));
		} else {
			snprintf(f->reason, sizeof(f->reason), "no IPv6 address");
			f->up = FALSE;
		}
	}
	if (o != NULL && qmi_message_wds_get_current_settings_output_get_mtu(o, &mtu,
	    NULL))
		f->mtu = mtu;
	if (o != NULL)
		qmi_message_wds_get_current_settings_output_unref(o);
	if (f->up)
		mlog("data: context %u IPv%d: %s%s%s, gateway %s, DNS %s %s, MTU %u",
		    c->n, c->step_family, f->addr, c->step_family == 4 ? "/" : "",
		    c->step_family == 4 ? f->mask : "", f->gw[0] ? f->gw : "-",
		    f->dns[0][0] ? f->dns[0] : "-", f->dns[1], f->mtu);
	if (c->step_family == 4 && proto_has(c, 6)) {
		c->step_family = 6;
		ctx_activate_next(c);
	} else
		ctx_activation_done(c);
}

static void
get_settings(struct context *c, struct family *f)
{
	QmiMessageWdsGetCurrentSettingsInput *in;

	in = qmi_message_wds_get_current_settings_input_new();
	qmi_message_wds_get_current_settings_input_set_requested_settings(in,
	    QMI_WDS_REQUESTED_SETTINGS_DNS_ADDRESS | QMI_WDS_REQUESTED_SETTINGS_IP_ADDRESS |
	    QMI_WDS_REQUESTED_SETTINGS_GATEWAY_INFO | QMI_WDS_REQUESTED_SETTINGS_MTU |
	    QMI_WDS_REQUESTED_SETTINGS_IP_FAMILY, NULL);
	qmi_client_wds_get_current_settings(f->wds, in, QMI_TIMEOUT, NULL,
	    (GAsyncReadyCallback)settings_ready, c);
	qmi_message_wds_get_current_settings_input_unref(in);
}

static void
start_ready(QmiClientWds *wds, GAsyncResult *res, struct context *c)
{
	struct family *f = c->step_family == 4 ? &c->f4 : &c->f6;
	QmiMessageWdsStartNetworkOutput *o;
	QmiWdsCallEndReason reason;
	QmiWdsVerboseCallEndReasonType vtype;
	gint16 vreason;
	GError *error = NULL;

	o = qmi_client_wds_start_network_finish(wds, res, &error);
	if (o != NULL && qmi_message_wds_start_network_output_get_result(o, &error) &&
	    qmi_message_wds_start_network_output_get_packet_data_handle(o, &f->handle,
	    NULL)) {
		f->up = TRUE;
		f->reason[0] = '\0';
		mlog("data: context %u IPv%d: session up (handle %#x)", c->n,
		    c->step_family, f->handle);
		qmi_message_wds_start_network_output_unref(o);
		get_settings(c, f);
		return;
	}
	snprintf(f->reason, sizeof(f->reason), "%s", qmi_err(error));
	if (o != NULL) {
		const char *cer = NULL, *ver = NULL;

		if (qmi_message_wds_start_network_output_get_call_end_reason(o, &reason,
		    NULL))
			cer = qmi_wds_call_end_reason_get_string(reason);
		if (qmi_message_wds_start_network_output_get_verbose_call_end_reason(o,
		    &vtype, &vreason, NULL))
			ver = qmi_wds_verbose_call_end_reason_get_string(vtype, vreason);
		snprintf(f->reason, sizeof(f->reason), "%s (call end %s%s%s)",
		    qmi_err(error), cer ? cer : "-", ver ? ", " : "", ver ? ver : "");
		qmi_message_wds_start_network_output_unref(o);
	}
	mlog("data: context %u IPv%d: Start Network: %s", c->n, c->step_family,
	    f->reason);
	g_clear_error(&error);
	if (c->step_family == 4 && proto_has(c, 6)) {
		c->step_family = 6;
		ctx_activate_next(c);
	} else
		ctx_activation_done(c);
}

static void
start_network(struct context *c, struct family *f)
{
	QmiMessageWdsStartNetworkInput *in;
	QmiWdsAuthentication auth = QMI_WDS_AUTHENTICATION_NONE;

	in = qmi_message_wds_start_network_input_new();
	if (c->apn[0])
		qmi_message_wds_start_network_input_set_apn(in, c->apn, NULL);
	if (!strcmp(c->auth, "chap"))
		auth = QMI_WDS_AUTHENTICATION_CHAP;
	else if (!strcmp(c->auth, "pap"))
		auth = QMI_WDS_AUTHENTICATION_PAP;
	if (c->user[0]) {
		qmi_message_wds_start_network_input_set_username(in, c->user, NULL);
		if (auth == QMI_WDS_AUTHENTICATION_NONE)
			auth = QMI_WDS_AUTHENTICATION_PAP | QMI_WDS_AUTHENTICATION_CHAP;
	}
	if (c->pass[0])
		qmi_message_wds_start_network_input_set_password(in, c->pass, NULL);
	qmi_message_wds_start_network_input_set_authentication_preference(in, auth,
	    NULL);
	qmi_message_wds_start_network_input_set_ip_family_preference(in,
	    c->step_family == 4 ? QMI_WDS_IP_FAMILY_IPV4 : QMI_WDS_IP_FAMILY_IPV6,
	    NULL);
	mlog("data: context %u IPv%d: Start Network, APN \"%s\"%s%s", c->n,
	    c->step_family, c->apn, c->user[0] ? ", user " : "", c->user);
	qmi_client_wds_start_network(f->wds, in, START_TIMEOUT, NULL,
	    (GAsyncReadyCallback)start_ready, c);
	qmi_message_wds_start_network_input_unref(in);
}

static void
family_ready(QmiClientWds *wds, GAsyncResult *res, struct context *c)
{
	struct family *f = c->step_family == 4 ? &c->f4 : &c->f6;
	QmiMessageWdsSetIpFamilyOutput *o;
	GError *error = NULL;

	/* not every modem has it; the family preference of Start Network is
	 * what counts (ModemManager goes on, too) */
	o = qmi_client_wds_set_ip_family_finish(wds, res, &error);
	if (o == NULL || !qmi_message_wds_set_ip_family_output_get_result(o, &error))
		mdbg("data: Set IP Family: %s", qmi_err(error));
	g_clear_error(&error);
	if (o != NULL)
		qmi_message_wds_set_ip_family_output_unref(o);
	start_network(c, f);
}

static void
bind_ready(QmiClientWds *wds, GAsyncResult *res, struct context *c)
{
	struct family *f = c->step_family == 4 ? &c->f4 : &c->f6;
	QmiMessageWdsBindMuxDataPortOutput *o;
	QmiMessageWdsSetIpFamilyInput *in;
	GError *error = NULL;

	o = qmi_client_wds_bind_mux_data_port_finish(wds, res, &error);
	if (o == NULL || !qmi_message_wds_bind_mux_data_port_output_get_result(o,
	    &error)) {
		snprintf(f->reason, sizeof(f->reason), "bind mux data port: %s",
		    qmi_err(error));
		mlog("data: context %u IPv%d: %s", c->n, c->step_family, f->reason);
		g_clear_error(&error);
		if (o != NULL)
			qmi_message_wds_bind_mux_data_port_output_unref(o);
		if (c->step_family == 4 && proto_has(c, 6)) {
			c->step_family = 6;
			ctx_activate_next(c);
		} else
			ctx_activation_done(c);
		return;
	}
	qmi_message_wds_bind_mux_data_port_output_unref(o);
	f->bound = TRUE;
	mdbg("data: context %u IPv%d: WDS client bound to mux %u", c->n,
	    c->step_family, c->mux);
	in = qmi_message_wds_set_ip_family_input_new();
	qmi_message_wds_set_ip_family_input_set_preference(in, c->step_family == 4 ?
	    QMI_WDS_IP_FAMILY_IPV4 : QMI_WDS_IP_FAMILY_IPV6, NULL);
	qmi_client_wds_set_ip_family(wds, in, QMI_TIMEOUT, NULL,
	    (GAsyncReadyCallback)family_ready, c);
	qmi_message_wds_set_ip_family_input_unref(in);
}

/* the network ended the session (Packet Service Status) */
static void
packet_service_status(QmiClientWds *wds, QmiIndicationWdsPacketServiceStatusOutput *o,
    struct context *c)
{
	struct family *f = wds == c->f4.wds ? &c->f4 : &c->f6;
	QmiWdsConnectionStatus st;
	QmiWdsCallEndReason reason = QMI_WDS_CALL_END_REASON_GENERIC_UNSPECIFIED;
	gboolean reconf;

	if (!qmi_indication_wds_packet_service_status_output_get_connection_status(o,
	    &st, &reconf, NULL))
		return;
	qmi_indication_wds_packet_service_status_output_get_call_end_reason(o, &reason,
	    NULL);
	mlog("data: context %u IPv%d: packet service %s%s%s", c->n,
	    f == &c->f4 ? 4 : 6, qmi_wds_connection_status_get_string(st),
	    st == QMI_WDS_CONNECTION_STATUS_DISCONNECTED ? ", " : "",
	    st == QMI_WDS_CONNECTION_STATUS_DISCONNECTED ?
	    qmi_wds_call_end_reason_get_string(reason) : "");
	if (st != QMI_WDS_CONNECTION_STATUS_DISCONNECTED || !f->up || c->activating)
		return;
	fam_clear(f);
	if (c->active && !c->f4.up && !c->f6.up)
		ctx_down(c, "the network ended it");
	else if (c->active) {
		run_hook(c, TRUE);
		ctx_active_changed(c);
	}
}

/* the WDS client of a family, allocated once */
static void
wds_alloc_ready(QmiDevice *d, GAsyncResult *res, struct context *c)
{
	struct family *f = c->step_family == 4 ? &c->f4 : &c->f6;
	GError *error = NULL;
	QmiClient *cl = qmi_device_allocate_client_finish(d, res, &error);

	if (cl == NULL) {
		snprintf(f->reason, sizeof(f->reason), "no WDS client: %s",
		    qmi_err(error));
		mlog("data: %s", f->reason);
		g_clear_error(&error);
		ctx_activation_done(c);
		return;
	}
	f->wds = QMI_CLIENT_WDS(cl);
	f->ind = g_signal_connect(f->wds, "packet-service-status",
	    G_CALLBACK(packet_service_status), c);
	ctx_activate_next(c);
}

/* the next step of the activation for c->step_family */
static void
ctx_activate_next(struct context *c)
{
	struct family *f = c->step_family == 4 ? &c->f4 : &c->f6;
	QmiMessageWdsBindMuxDataPortInput *in;

	if (f->wds == NULL) {
		if (c->step_family == 4 && c->n == 1 && wds_spare != NULL) {
			f->wds = wds_spare;
			wds_spare = NULL;
			f->ind = g_signal_connect(f->wds, "packet-service-status",
			    G_CALLBACK(packet_service_status), c);
		} else {
			qmi_device_allocate_client(dev, QMI_SERVICE_WDS, QMI_CID_NONE,
			    QMI_TIMEOUT, NULL, (GAsyncReadyCallback)wds_alloc_ready, c);
			return;
		}
	}
	if (f->bound) {
		start_network(c, f);
		return;
	}
	in = qmi_message_wds_bind_mux_data_port_input_new();
	qmi_message_wds_bind_mux_data_port_input_set_endpoint_info(in,
	    QMI_DATA_ENDPOINT_TYPE_EMBEDDED, 1, NULL);
	qmi_message_wds_bind_mux_data_port_input_set_mux_id(in, c->mux, NULL);
	qmi_message_wds_bind_mux_data_port_input_set_client_type(in,
	    QMI_WDS_CLIENT_TYPE_TETHERED, NULL);
	qmi_client_wds_bind_mux_data_port(f->wds, in, QMI_TIMEOUT, NULL,
	    (GAsyncReadyCallback)bind_ready, c);
	qmi_message_wds_bind_mux_data_port_input_unref(in);
}

static const char *
ctx_cannot(const struct context *c)
{
	if (!cm_powered)
		return "ConnectionManager.Powered is off";
	if (dp != DP_READY)
		return "the modem's data port is not set up";
	if (!sim_ready())
		return "the SIM is not ready";
	if (!attached())
		return "not attached to a network";
	if (!strcmp(M.reg_status, "roaming") && !cm_roaming)
		return "roaming, and RoamingAllowed is off";
	if (!dopt.no_ifcheck && if_nametoindex(c->ifname) == 0)
		return "no QMAP interface for its mux id (qcom_ipa, hw.qcom_ipa.muxes)";
	return NULL;
}

static void
ctx_activate(struct context *c, GDBusMethodInvocation *inv)
{
	const char *why;

	if (c->active) {
		if (inv != NULL)
			g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	if (c->activating || c->deactivating) {
		if (inv != NULL)
			ofono_reply_error(inv, OFONO_ERROR_IN_PROGRESS, "busy");
		return;
	}
	if ((why = ctx_cannot(c)) != NULL) {
		mlog("data: context %u not activated: %s", c->n, why);
		if (inv != NULL)
			ofono_reply_error(inv, OFONO_ERROR_NOT_AVAILABLE, why);
		return;
	}
	c->inv = inv;
	c->activating = TRUE;
	c->f4.reason[0] = c->f6.reason[0] = '\0';
	c->step_family = proto_has(c, 4) ? 4 : 6;
	mlog("data: context %u: activating (%s) on %s, mux %u", c->n, c->proto,
	    c->ifname, c->mux);
	ctx_activate_next(c);
}

static void
stop_ready(QmiClientWds *wds, GAsyncResult *res, struct context *c)
{
	QmiMessageWdsStopNetworkOutput *o;
	GError *error = NULL;

	o = qmi_client_wds_stop_network_finish(wds, res, &error);
	if (o == NULL || !qmi_message_wds_stop_network_output_get_result(o, &error))
		mlog("data: context %u: Stop Network: %s", c->n, qmi_err(error));
	g_clear_error(&error);
	if (o != NULL)
		qmi_message_wds_stop_network_output_unref(o);
}

/* the context down; Stop Network for what is still up */
static void
ctx_down(struct context *c, const char *why)
{
	struct family *fs[2] = { &c->f4, &c->f6 };
	QmiMessageWdsStopNetworkInput *in;

	for (int i = 0; i < 2; i++) {
		if (!fs[i]->up || fs[i]->wds == NULL)
			continue;
		in = qmi_message_wds_stop_network_input_new();
		qmi_message_wds_stop_network_input_set_packet_data_handle(in,
		    fs[i]->handle, NULL);
		qmi_client_wds_stop_network(fs[i]->wds, in, QMI_TIMEOUT, NULL,
		    (GAsyncReadyCallback)stop_ready, c);
		qmi_message_wds_stop_network_input_unref(in);
	}
	if (c->active)
		run_hook(c, FALSE);
	fam_clear(&c->f4);
	fam_clear(&c->f6);
	if (c->active) {
		c->active = FALSE;
		mlog("data: context %u inactive (%s)", c->n, why);
		ctx_active_changed(c);
	}
}

/* --- D-Bus: methods ------------------------------------------------------------ */

static struct context *
ctx_by_path(const char *path)
{
	for (guint i = 0; i < NCTX; i++)
		if (ctxs[i] != NULL && !strcmp(ctxs[i]->path, path))
			return ctxs[i];
	return NULL;
}

static void
set_str(char **slot, const char *v)
{
	g_free(*slot);
	*slot = g_strdup(v);
}

static void
ctx_method(GDBusConnection *conn, const char *sender, const char *path,
    const char *iface, const char *method, GVariant *args,
    GDBusMethodInvocation *inv, gpointer data)
{
	struct context *c = ctx_by_path(path);
	const char *name;
	GVariant *v;

	(void)conn; (void)iface; (void)data;
	mdbg("D-Bus: ConnectionContext.%s from %s", method, sender);
	if (c == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_AVAILABLE, path);
		return;
	}
	if (!strcmp(method, "GetProperties")) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})",
		    ctx_dict(c)));
		return;
	}
	if (strcmp(method, "SetProperty") != 0) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
		return;
	}
	g_variant_get(args, "(&sv)", &name, &v);
	if (!strcmp(name, "Active") && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
		if (g_variant_get_boolean(v))
			ctx_activate(c, inv);
		else {
			ctx_down(c, "on request");
			g_dbus_method_invocation_return_value(inv, NULL);
		}
		g_variant_unref(v);
		return;
	}
	if (!g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
		ofono_reply_error(inv, OFONO_ERROR_INVALID_ARGS, name);
		g_variant_unref(v);
		return;
	}
	if (c->active || c->activating) {
		ofono_reply_error(inv, OFONO_ERROR_IN_PROGRESS, "the context is active");
		g_variant_unref(v);
		return;
	}
	{
		const char *s = g_variant_get_string(v, NULL);
		char **slot = NULL;

		if (!strcmp(name, "AccessPointName"))
			slot = &c->apn;
		else if (!strcmp(name, "Username"))
			slot = &c->user;
		else if (!strcmp(name, "Password"))
			slot = &c->pass;
		else if (!strcmp(name, "Name"))
			slot = &c->name;
		else if (!strcmp(name, "AuthenticationMethod") && (!strcmp(s, "chap") ||
		    !strcmp(s, "pap") || !strcmp(s, "none")))
			slot = &c->auth;
		else if (!strcmp(name, "Protocol") && (!strcmp(s, "ip") ||
		    !strcmp(s, "ipv6") || !strcmp(s, "dual")))
			slot = &c->proto;
		else if (!strcmp(name, "Type") && (!strcmp(s, "internet") ||
		    !strcmp(s, "mms") || !strcmp(s, "ia")))
			slot = &c->type;
		if (slot == NULL) {
			ofono_reply_error(inv, OFONO_ERROR_INVALID_ARGS, name);
		} else {
			set_str(slot, s);
			mlog("data: context %u: %s %s", c->n, name,
			    slot == &c->pass ? "(set)" : s);
			g_dbus_method_invocation_return_value(inv, NULL);
			ctx_changed(c, name, g_variant_new_string(s));
		}
	}
	g_variant_unref(v);
}

static const GDBusInterfaceVTable ctx_vtable = { ctx_method, NULL, NULL, { 0 } };

static struct context *
ctx_new(const char *type)
{
	struct context *c;
	GError *error = NULL;
	guint i;

	for (i = 0; i < NCTX && ctxs[i] != NULL; i++)
		;
	if (i == NCTX)
		return NULL;
	c = g_new0(struct context, 1);
	c->n = i + 1;
	c->mux = (guint8)(i + 1);
	snprintf(c->ifname, sizeof(c->ifname), "%s%u", dopt.ifprefix, i);
	c->path = g_strdup_printf("%s/context%u", M.path, c->n);
	c->name = g_strdup(!strcmp(type, "mms") ? "MMS" : "Internet");
	c->type = g_strdup(type);
	c->apn = g_strdup("");
	c->user = g_strdup("");
	c->pass = g_strdup("");
	c->auth = g_strdup("chap");
	c->proto = g_strdup("ip");
	if (bus != NULL) {
		c->reg = g_dbus_connection_register_object(bus, c->path,
		    g_dbus_node_info_lookup_interface(node_info,
		    "org.ofono.ConnectionContext"), &ctx_vtable, NULL, NULL, &error);
		if (c->reg == 0) {
			mlog("D-Bus: %s: %s", c->path, error->message);
			g_error_free(error);
		}
	}
	ctxs[i] = c;
	return c;
}

static void
ctx_free(struct context *c)
{
	if (c->f4.wds && c->f4.ind)
		g_signal_handler_disconnect(c->f4.wds, c->f4.ind);
	if (c->f6.wds && c->f6.ind)
		g_signal_handler_disconnect(c->f6.wds, c->f6.ind);
	if (c->reg)
		g_dbus_connection_unregister_object(bus, c->reg);
	ctxs[c->n - 1] = NULL;
	g_free(c->path); g_free(c->name); g_free(c->type); g_free(c->apn);
	g_free(c->user); g_free(c->pass); g_free(c->auth); g_free(c->proto);
	g_free(c);
}

static void
cm_method(GDBusConnection *conn, const char *sender, const char *path,
    const char *iface, const char *method, GVariant *args,
    GDBusMethodInvocation *inv, gpointer data)
{
	(void)conn; (void)path; (void)iface; (void)data;
	mdbg("D-Bus: ConnectionManager.%s from %s", method, sender);
	if (!strcmp(method, "GetProperties")) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})",
		    cm_dict()));
	} else if (!strcmp(method, "GetContexts")) {
		GVariantBuilder b;

		g_variant_builder_init(&b, G_VARIANT_TYPE("a(oa{sv})"));
		for (guint i = 0; i < NCTX; i++)
			if (ctxs[i] != NULL)
				g_variant_builder_add(&b, "(o@a{sv})", ctxs[i]->path,
				    ctx_dict(ctxs[i]));
		g_dbus_method_invocation_return_value(inv, g_variant_new("(a(oa{sv}))",
		    &b));
	} else if (!strcmp(method, "SetProperty")) {
		const char *name;
		GVariant *v;

		g_variant_get(args, "(&sv)", &name, &v);
		if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
		    (!strcmp(name, "Powered") || !strcmp(name, "RoamingAllowed"))) {
			gboolean on = g_variant_get_boolean(v);

			if (!strcmp(name, "Powered")) {
				cm_powered = on;
				if (!on)
					for (guint i = 0; i < NCTX; i++)
						if (ctxs[i] != NULL)
							ctx_down(ctxs[i], "Powered off");
			} else
				cm_roaming = on;
			g_dbus_method_invocation_return_value(inv, NULL);
			cm_changed(name, g_variant_new_boolean(on));
			maybe_autoconnect();
		} else
			ofono_reply_error(inv, OFONO_ERROR_INVALID_ARGS, name);
		g_variant_unref(v);
	} else if (!strcmp(method, "AddContext")) {
		const char *type;
		struct context *c;

		g_variant_get(args, "(&s)", &type);
		if (strcmp(type, "internet") && strcmp(type, "mms") && strcmp(type, "ia")) {
			ofono_reply_error(inv, OFONO_ERROR_INVALID_FORMAT, type);
			return;
		}
		if ((c = ctx_new(type)) == NULL) {
			ofono_reply_error(inv, OFONO_ERROR_FAILED, "no more contexts");
			return;
		}
		g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", c->path));
		emit(M.path, "org.ofono.ConnectionManager", "ContextAdded",
		    g_variant_new("(o@a{sv})", c->path, ctx_dict(c)));
	} else if (!strcmp(method, "RemoveContext")) {
		const char *p;
		struct context *c;

		g_variant_get(args, "(&o)", &p);
		if ((c = ctx_by_path(p)) == NULL || c->activating) {
			ofono_reply_error(inv, OFONO_ERROR_INVALID_ARGS, p);
			return;
		}
		ctx_down(c, "removed");
		{
			char *gone = g_strdup(c->path);

			ctx_free(c);
			g_dbus_method_invocation_return_value(inv, NULL);
			emit(M.path, "org.ofono.ConnectionManager", "ContextRemoved",
			    g_variant_new("(o)", gone));
			g_free(gone);
		}
	} else if (!strcmp(method, "DeactivateAll")) {
		for (guint i = 0; i < NCTX; i++)
			if (ctxs[i] != NULL)
				ctx_down(ctxs[i], "DeactivateAll");
		g_dbus_method_invocation_return_value(inv, NULL);
	} else
		ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, method);
}

static const GDBusInterfaceVTable cm_vtable = { cm_method, NULL, NULL, { 0 } };

/* --- the data port: DPM, WDA ------------------------------------------------------ */

static void
dp_done(enum dp_state st, const char *msg)
{
	dp = st;
	mlog("data: %s", msg);
	ofono_interfaces_changed();
	maybe_autoconnect();
}

static void wda_set_next(void);

static void
wda_verify_ready(QmiClientWda *c, GAsyncResult *res, gpointer data)
{
	QmiMessageWdaGetDataFormatOutput *o;
	QmiWdaLinkLayerProtocol llp = QMI_WDA_LINK_LAYER_PROTOCOL_UNKNOWN;
	QmiWdaDataAggregationProtocol ul = 0, dl = 0;
	guint32 want = wda_protocols[wda_try];
	GError *error = NULL;
	char msg[128];

	(void)data;
	o = qmi_client_wda_get_data_format_finish(c, res, &error);
	if (o == NULL || !qmi_message_wda_get_data_format_output_get_result(o, &error)) {
		mlog("data: WDA data format not read back (%s): taking %s as set",
		    qmi_err(error), wda_names[wda_try]);
		g_clear_error(&error);
		if (o != NULL)
			qmi_message_wda_get_data_format_output_unref(o);
		snprintf(msg, sizeof(msg), "data port ready: raw IP, %s",
		    wda_names[wda_try]);
		dp_done(DP_READY, msg);
		return;
	}
	qmi_message_wda_get_data_format_output_get_link_layer_protocol(o, &llp, NULL);
	qmi_message_wda_get_data_format_output_get_uplink_data_aggregation_protocol(o,
	    &ul, NULL);
	qmi_message_wda_get_data_format_output_get_downlink_data_aggregation_protocol(o,
	    &dl, NULL);
	qmi_message_wda_get_data_format_output_unref(o);
	if (llp != QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP || ul != want || dl != want) {
		mlog("data: WDA: %s not taken (link %s, uplink %s, downlink %s)",
		    wda_names[wda_try], qmi_wda_link_layer_protocol_get_string(llp),
		    qmi_wda_data_aggregation_protocol_get_string(ul),
		    qmi_wda_data_aggregation_protocol_get_string(dl));
		wda_try++;
		wda_set_next();
		return;
	}
	snprintf(msg, sizeof(msg), "data port ready: raw IP, %s both ways",
	    wda_names[wda_try]);
	dp_done(DP_READY, msg);
}

static void
wda_get(GAsyncReadyCallback cb)
{
	QmiMessageWdaGetDataFormatInput *in = NULL;

	if (wda_endpoint) {
		in = qmi_message_wda_get_data_format_input_new();
		qmi_message_wda_get_data_format_input_set_endpoint_info(in,
		    QMI_DATA_ENDPOINT_TYPE_EMBEDDED, 1, NULL);
	}
	qmi_client_wda_get_data_format(wda, in, QMI_TIMEOUT, NULL, cb, NULL);
	if (in != NULL)
		qmi_message_wda_get_data_format_input_unref(in);
}

static void
wda_set_ready(QmiClientWda *c, GAsyncResult *res, gpointer data)
{
	QmiMessageWdaSetDataFormatOutput *o;
	GError *error = NULL;

	(void)data;
	o = qmi_client_wda_set_data_format_finish(c, res, &error);
	if (o == NULL || !qmi_message_wda_set_data_format_output_get_result(o, &error)) {
		mlog("data: WDA: %s refused: %s", wda_names[wda_try], qmi_err(error));
		g_clear_error(&error);
		if (o != NULL)
			qmi_message_wda_set_data_format_output_unref(o);
		wda_try++;
		wda_set_next();
		return;
	}
	qmi_message_wda_set_data_format_output_unref(o);
	wda_get((GAsyncReadyCallback)wda_verify_ready);
}

static void
wda_set_next(void)
{
	QmiMessageWdaSetDataFormatInput *in;
	guint32 p;

	if (wda_try >= wda_end) {
		dp_done(DP_FAILED, "no data port: the modem took none of the QMAP "
		    "formats (WDA)");
		return;
	}
	p = wda_protocols[wda_try];
	mlog("data: WDA: Set Data Format raw IP, %s, %u datagrams / %u bytes%s",
	    wda_names[wda_try], DL_DATAGRAMS, DL_SIZE,
	    wda_endpoint ? ", endpoint EMBEDDED 1" : "");
	in = qmi_message_wda_set_data_format_input_new();
	qmi_message_wda_set_data_format_input_set_link_layer_protocol(in,
	    QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP, NULL);
	qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_protocol(in,
	    p, NULL);
	qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_protocol(in,
	    p, NULL);
	qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_datagrams(
	    in, DL_DATAGRAMS, NULL);
	qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_size(in,
	    DL_SIZE, NULL);
	if (wda_endpoint)
		qmi_message_wda_set_data_format_input_set_endpoint_info(in,
		    QMI_DATA_ENDPOINT_TYPE_EMBEDDED, 1, NULL);
	qmi_client_wda_set_data_format(wda, in, QMI_TIMEOUT, NULL,
	    (GAsyncReadyCallback)wda_set_ready, NULL);
	qmi_message_wda_set_data_format_input_unref(in);
}

static void
wda_first_ready(QmiClientWda *c, GAsyncResult *res, gpointer data)
{
	QmiMessageWdaGetDataFormatOutput *o;
	QmiWdaLinkLayerProtocol llp = 0;
	QmiWdaDataAggregationProtocol ul = 0, dl = 0;
	GError *error = NULL;

	(void)data;
	o = qmi_client_wda_get_data_format_finish(c, res, &error);
	if (o == NULL || !qmi_message_wda_get_data_format_output_get_result(o, &error)) {
		/* as ModemManager: a modem missing the endpoint info says so */
		if (!wda_endpoint && g_error_matches(error, QMI_PROTOCOL_ERROR,
		    QMI_PROTOCOL_ERROR_MISSING_ARGUMENT)) {
			g_clear_error(&error);
			if (o != NULL)
				qmi_message_wda_get_data_format_output_unref(o);
			wda_endpoint = TRUE;
			wda_get((GAsyncReadyCallback)wda_first_ready);
			return;
		}
		mdbg("data: WDA Get Data Format: %s", qmi_err(error));
		g_clear_error(&error);
	} else {
		qmi_message_wda_get_data_format_output_get_link_layer_protocol(o, &llp,
		    NULL);
		qmi_message_wda_get_data_format_output_get_uplink_data_aggregation_protocol(
		    o, &ul, NULL);
		qmi_message_wda_get_data_format_output_get_downlink_data_aggregation_protocol(
		    o, &dl, NULL);
		mlog("data: WDA data format now: link %s, uplink %s, downlink %s",
		    qmi_wda_link_layer_protocol_get_string(llp),
		    qmi_wda_data_aggregation_protocol_get_string(ul),
		    qmi_wda_data_aggregation_protocol_get_string(dl));
	}
	if (o != NULL)
		qmi_message_wda_get_data_format_output_unref(o);
	wda_set_next();
}

static void
wda_start(void)
{
	wda_endpoint = FALSE;
	wda_try = 0;
	wda_end = G_N_ELEMENTS(wda_protocols);
	for (int i = 0; dopt.wda != NULL && i < (int)G_N_ELEMENTS(wda_names); i++)
		if (!strcmp(dopt.wda, wda_names[i])) {
			wda_try = i;
			wda_end = i + 1;
			mlog("data: WDA: only %s (-W)", wda_names[i]);
		}
	wda_get((GAsyncReadyCallback)wda_first_ready);
}

static void
dpm_ready(QmiClientDpm *c, GAsyncResult *res, gpointer data)
{
	QmiMessageDpmOpenPortOutput *o;
	GError *error = NULL;

	(void)data;
	o = qmi_client_dpm_open_port_finish(c, res, &error);
	if (o == NULL || !qmi_message_dpm_open_port_output_get_result(o, &error))
		mlog("data: DPM Open Port failed (%s), going on", qmi_err(error));
	else
		mlog("data: DPM port open");
	g_clear_error(&error);
	if (o != NULL)
		qmi_message_dpm_open_port_output_unref(o);
	wda_start();
}

/* the IPA's endpoint ids facing the modem (qcom_ipa's sysctls) */
static gboolean
ipa_endpoints(guint32 *rx, guint32 *tx)
{
	if (dopt.rx_ep != 0 && dopt.tx_ep != 0) {
		*rx = dopt.rx_ep;
		*tx = dopt.tx_ep;
		return TRUE;
	}
#ifdef __FreeBSD__
	{
		char name[96];
		unsigned char v;
		size_t len = sizeof(v);

		snprintf(name, sizeof(name), "%s.modem.rx_endpoint_id", dopt.ipa_sysctl);
		if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
			return FALSE;
		*rx = v;
		len = sizeof(v);
		snprintf(name, sizeof(name), "%s.modem.tx_endpoint_id", dopt.ipa_sysctl);
		if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
			return FALSE;
		*tx = v;
		return TRUE;
	}
#else
	return FALSE;
#endif
}

static void
dpm_open_port(void)
{
	QmiMessageDpmOpenPortInputHardwareDataPortsElement e;
	QmiMessageDpmOpenPortInput *in;
	GArray *a;
	guint32 rx, tx;

	if (dpm == NULL) {
		mlog("data: the modem has no DPM service");
		wda_start();
		return;
	}
	if (!ipa_endpoints(&rx, &tx)) {
		mlog("data: no IPA endpoint ids (%s.modem.*_endpoint_id, -E): DPM port "
		    "not opened", dopt.ipa_sysctl);
		wda_start();
		return;
	}
	/* the modem's RX endpoint is the IPA's TX one, and the other way round */
	e.endpoint_type = QMI_DATA_ENDPOINT_TYPE_EMBEDDED;
	e.interface_number = 1;
	e.rx_endpoint_number = tx;
	e.tx_endpoint_number = rx;
	a = g_array_new(FALSE, FALSE, sizeof(e));
	g_array_append_val(a, e);
	in = qmi_message_dpm_open_port_input_new();
	qmi_message_dpm_open_port_input_set_hardware_data_ports(in, a, NULL);
	mlog("data: DPM Open Port: modem RX endpoint %u, TX endpoint %u", tx, rx);
	qmi_client_dpm_open_port(dpm, in, QMI_TIMEOUT, NULL,
	    (GAsyncReadyCallback)dpm_ready, NULL);
	qmi_message_dpm_open_port_input_unref(in);
	g_array_unref(a);
}

/* --- clients ----------------------------------------------------------------------------- */

static const struct {
	QmiService	svc;
	const char	*name;
	gboolean	required;
} data_svcs[3] = {
	{ QMI_SERVICE_WDS, "WDS", TRUE },
	{ QMI_SERVICE_WDA, "WDA", TRUE },
	{ QMI_SERVICE_DPM, "DPM", FALSE },
};

static void data_alloc(guint i);

static gboolean
data_alloc_again(gpointer p)
{
	data_alloc(GPOINTER_TO_UINT(p));
	return G_SOURCE_REMOVE;
}

static void
data_alloc_ready(QmiDevice *d, GAsyncResult *res, gpointer p)
{
	guint i = GPOINTER_TO_UINT(p);
	GError *error = NULL;
	QmiClient *c = qmi_device_allocate_client_finish(d, res, &error);

	if (c == NULL) {
		/* DPM is optional: one try, then on without it */
		if (data_svcs[i].required && ++alloc_tries[i] < ALLOC_TRIES) {
			if (alloc_tries[i] == 1)
				mlog("data: no %s client yet (%s): again every 5 s",
				    data_svcs[i].name, qmi_err(error));
			g_timeout_add_seconds(5, data_alloc_again, GUINT_TO_POINTER(i));
			g_clear_error(&error);
			return;
		}
		mlog("data: no %s client: %s", data_svcs[i].name, qmi_err(error));
		g_clear_error(&error);
		if (data_svcs[i].required) {
			dp_done(DP_FAILED, "no mobile data (WDS or WDA missing)");
			return;
		}
	} else {
		mlog("data: %s client %u", data_svcs[i].name, qmi_client_get_cid(c));
		if (data_svcs[i].svc == QMI_SERVICE_WDS)
			wds_spare = QMI_CLIENT_WDS(c);
		else if (data_svcs[i].svc == QMI_SERVICE_WDA)
			wda = QMI_CLIENT_WDA(c);
		else
			dpm = QMI_CLIENT_DPM(c);
	}
	if (i + 1 < G_N_ELEMENTS(data_svcs))
		data_alloc(i + 1);
	else
		dpm_open_port();
}

static void
data_alloc(guint i)
{
	qmi_device_allocate_client(dev, data_svcs[i].svc, QMI_CID_NONE, QMI_TIMEOUT,
	    NULL, (GAsyncReadyCallback)data_alloc_ready, GUINT_TO_POINTER(i));
}

void
data_start(QmiDevice *d)
{
	if (dopt.disabled)
		return;
	dev = d;
	dp = DP_SETUP;
	data_alloc(0);
}

/* autoconnect: the first context, once everything is there */
static void
maybe_autoconnect(void)
{
	struct context *c = ctxs[0];

	if (!autoconnect || c == NULL || c->active || c->activating ||
	    c->deactivating || ctx_cannot(c) != NULL)
		return;
	mlog("data: autoconnect");
	ctx_activate(c, NULL);
}

/* SIGTERM: what is up comes down (the interfaces' addresses with it) */
void
data_stop(void)
{
	for (guint i = 0; i < NCTX; i++)
		if (ctxs[i] != NULL)
			ctx_down(ctxs[i], "stopping");
}

/* --- the user's file ------------------------------------------------------------ */

static void
read_conf(struct context *c)
{
	char line[256], *v, *e;
	FILE *f;

	if (dopt.conf == NULL || (f = fopen(dopt.conf, "r")) == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] == '#' || (v = strchr(line, '=')) == NULL)
			continue;
		*v++ = '\0';
		for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' ||
		    e[-1] == ' ' || e[-1] == '\t'); e--)
			;
		*e = '\0';
		if (!strcmp(line, "apn"))
			set_str(&c->apn, v);
		else if (!strcmp(line, "username"))
			set_str(&c->user, v);
		else if (!strcmp(line, "password"))
			set_str(&c->pass, v);
		else if (!strcmp(line, "auth") && (!strcmp(v, "chap") ||
		    !strcmp(v, "pap") || !strcmp(v, "none")))
			set_str(&c->auth, v);
		else if (!strcmp(line, "protocol") && (!strcmp(v, "ip") ||
		    !strcmp(v, "ipv6") || !strcmp(v, "dual")))
			set_str(&c->proto, v);
		else if (!strcmp(line, "roaming"))
			cm_roaming = atoi(v) != 0;
		else if (!strcmp(line, "autoconnect"))
			autoconnect = atoi(v) != 0;
	}
	fclose(f);
	mlog("data: %s: APN \"%s\", %s, %s%s%s", dopt.conf, c->apn, c->auth,
	    c->proto, cm_roaming ? ", roaming allowed" : "",
	    autoconnect ? ", autoconnect" : "");
}

int
data_init(GDBusConnection *conn)
{
	GError *error = NULL;
	struct context *c;

	if (dopt.disabled)
		return 0;
	bus = conn;
	node_info = g_dbus_node_info_new_for_xml(introspection, &error);
	g_assert_no_error(error);
	if (g_dbus_connection_register_object(bus, M.path,
	    g_dbus_node_info_lookup_interface(node_info, "org.ofono.ConnectionManager"),
	    &cm_vtable, NULL, NULL, &error) == 0) {
		mlog("D-Bus: ConnectionManager: %s", error->message);
		g_error_free(error);
		return -1;
	}
	last_attached = attached();
	last_bearer = bearer();
	c = ctx_new("internet");
	read_conf(c);
	return 0;
}
