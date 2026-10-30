/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-modemd's modem side: QMI through libqmi-glib (1.34, Leste's
 * version; the messages and their TLVs are libqmi's, data/qmi-service-
 * {dms,uim,nas,wms,voice}.json). What it asks of the modem follows
 * KolibriOS for ARM's kernel/modem/phone.c, which does the same over raw
 * QMI (docs/pro1x-modem.md there):
 *
 *   DMS    manufacturer, model, revision, IMEI; the operating mode (online
 *          at the start, Modem.Online)
 *   UIM    the card's status and its indication; the PIN (the PIN file,
 *          once per boot, or SimManager.EnterPin)
 *   NAS    the serving system and its indication (registration, operator,
 *          technology, LAC/TAC, cell), the signal every few seconds, the
 *          network's time
 *   WMS    routes: class 2 on the SIM, the others in the modem, store and
 *          notify; the event report; the unread messages read at the start
 *          and on their indication (then tagged read); SMS-SUBMIT PDUs sent
 *          with Raw Send
 *   VOICE  call notifications (All Call Status), dial, answer, end, DTMF
 *          (continuous start and stop, as ModemManager's QMI calls do)
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modemd.h"
#include "sms.h"

#define	TIMEOUT		10	/* seconds per request */
#define	ALLOC_TRIES	24	/* a service not up yet: again every 5 s */
#define	SMS_PARTS_MAX	6
#define	CONCAT_TIMEOUT	(10 * 60)	/* seconds: the missing parts given up */
#define	DTMF_ON_MS	250
#define	DTMF_OFF_MS	100

static QmiDevice *dev;
static QmiClient *cl_dms, *cl_uim, *cl_nas, *cl_wms, *cl_voice;
static guint call_counter;
static guint signal_timer;
static gboolean pin_busy;

#define	DMS	QMI_CLIENT_DMS(cl_dms)
#define	UIM	QMI_CLIENT_UIM(cl_uim)
#define	NAS	QMI_CLIENT_NAS(cl_nas)
#define	WMS	QMI_CLIENT_WMS(cl_wms)
#define	VOICE	QMI_CLIENT_VOICE(cl_voice)

static void wms_list(QmiWmsStorageType storage);
static void uim_card_status(void);
static void sim_ready_changed(void);

static void
set_str(char **dst, const char *src)
{
	if (g_strcmp0(*dst, src) == 0)
		return;
	g_free(*dst);
	*dst = src != NULL ? g_strdup(src) : NULL;
}

/* a reply's error for the D-Bus side and the log */
static void
fail(GDBusMethodInvocation *inv, const char *what, GError *error)
{
	mlog("%s: %s", what, error ? error->message : "failed");
	if (inv != NULL) {
		const char *name = OFONO_ERROR_FAILED;

		if (error != NULL && error->domain == QMI_PROTOCOL_ERROR &&
		    (error->code == QMI_PROTOCOL_ERROR_INCORRECT_PIN ||
		    error->code == QMI_PROTOCOL_ERROR_PIN_BLOCKED))
			name = OFONO_ERROR_INCORRECT_PASSWORD;
		ofono_reply_error(inv, name, error ? error->message : what);
	}
	g_clear_error(&error);
}

/* --- DMS ---------------------------------------------------------------- */

static void
dms_str_ready(QmiClientDms *c, GAsyncResult *res, gpointer which)
{
	GError *error = NULL;
	const char *s = NULL, *prop = which;
	char **dst = NULL;

	if (!strcmp(prop, "Manufacturer")) {
		QmiMessageDmsGetManufacturerOutput *o =
		    qmi_client_dms_get_manufacturer_finish(c, res, &error);

		if (o && qmi_message_dms_get_manufacturer_output_get_result(o, &error))
			qmi_message_dms_get_manufacturer_output_get_manufacturer(o, &s, NULL);
		dst = &M.manufacturer;
		if (s)
			set_str(dst, s);
		if (o)
			qmi_message_dms_get_manufacturer_output_unref(o);
	} else if (!strcmp(prop, "Model")) {
		QmiMessageDmsGetModelOutput *o = qmi_client_dms_get_model_finish(c, res, &error);

		if (o && qmi_message_dms_get_model_output_get_result(o, &error))
			qmi_message_dms_get_model_output_get_model(o, &s, NULL);
		dst = &M.model;
		if (s)
			set_str(dst, s);
		if (o)
			qmi_message_dms_get_model_output_unref(o);
	} else if (!strcmp(prop, "Revision")) {
		QmiMessageDmsGetRevisionOutput *o =
		    qmi_client_dms_get_revision_finish(c, res, &error);

		if (o && qmi_message_dms_get_revision_output_get_result(o, &error))
			qmi_message_dms_get_revision_output_get_revision(o, &s, NULL);
		dst = &M.revision;
		if (s)
			set_str(dst, s);
		if (o)
			qmi_message_dms_get_revision_output_unref(o);
	} else {
		QmiMessageDmsGetIdsOutput *o = qmi_client_dms_get_ids_finish(c, res, &error);

		if (o && qmi_message_dms_get_ids_output_get_result(o, &error))
			qmi_message_dms_get_ids_output_get_imei(o, &s, NULL);
		dst = &M.serial;
		if (s)
			set_str(dst, s);
		if (o)
			qmi_message_dms_get_ids_output_unref(o);
	}
	if (error != NULL) {
		mlog("dms: %s: %s", prop, error->message);
		g_error_free(error);
		return;
	}
	/* the IMEI is the phone's identity: not in the log */
	mlog("dms: %s %s", prop, !strcmp(prop, "Serial") ? "(read)" : *dst ? *dst : "?");
	ofono_modem_changed(prop);
}

static void
online_changed(gboolean on)
{
	if (M.online == on)
		return;
	M.online = on;
	mlog("dms: %s", on ? "online" : "offline (low power)");
	ofono_modem_changed("Online");
	ofono_interfaces_changed();
	if (!on) {
		M.reg_status = "unregistered";
		M.strength = -1;
		ofono_netreg_changed("Status");
	}
}

struct mode_ctx {
	GDBusMethodInvocation *inv;
	gboolean on;
};

static void dms_get_mode(void);

static void
dms_set_mode_ready(QmiClientDms *c, GAsyncResult *res, struct mode_ctx *x)
{
	GError *error = NULL;
	QmiMessageDmsSetOperatingModeOutput *o =
	    qmi_client_dms_set_operating_mode_finish(c, res, &error);

	if (o == NULL || !qmi_message_dms_set_operating_mode_output_get_result(o, &error)) {
		fail(x->inv, "dms: set operating mode", error);
	} else {
		online_changed(x->on);
		if (x->inv)
			g_dbus_method_invocation_return_value(x->inv, NULL);
		/* what the modem says it is now */
		dms_get_mode();
	}
	if (o)
		qmi_message_dms_set_operating_mode_output_unref(o);
	g_free(x);
}

void
qmi_set_online(gboolean on, GDBusMethodInvocation *inv)
{
	QmiMessageDmsSetOperatingModeInput *in;
	struct mode_ctx *x;

	if (cl_dms == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_AVAILABLE, "no DMS");
		return;
	}
	x = g_new0(struct mode_ctx, 1);
	x->inv = inv;
	x->on = on;
	in = qmi_message_dms_set_operating_mode_input_new();
	qmi_message_dms_set_operating_mode_input_set_mode(in,
	    on ? QMI_DMS_OPERATING_MODE_ONLINE : QMI_DMS_OPERATING_MODE_LOW_POWER, NULL);
	mlog("dms: operating mode -> %s", on ? "online" : "low power");
	qmi_client_dms_set_operating_mode(DMS, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)dms_set_mode_ready, x);
	qmi_message_dms_set_operating_mode_input_unref(in);
}

static gboolean mode_checked;

static void
dms_get_mode_ready(QmiClientDms *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	QmiDmsOperatingMode mode = QMI_DMS_OPERATING_MODE_UNKNOWN;
	QmiMessageDmsGetOperatingModeOutput *o =
	    qmi_client_dms_get_operating_mode_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_dms_get_operating_mode_output_get_result(o, &error)) {
		fail(NULL, "dms: operating mode", error);
	} else {
		qmi_message_dms_get_operating_mode_output_get_mode(o, &mode, NULL);
		mlog("dms: operating mode %s", qmi_dms_operating_mode_get_string(mode));
		online_changed(mode == QMI_DMS_OPERATING_MODE_ONLINE);
		/* the first time: online if wanted */
		if (!mode_checked && opt.online && mode != QMI_DMS_OPERATING_MODE_ONLINE)
			qmi_set_online(TRUE, NULL);
		mode_checked = TRUE;
	}
	if (o)
		qmi_message_dms_get_operating_mode_output_unref(o);
}

static void
dms_get_mode(void)
{
	qmi_client_dms_get_operating_mode(DMS, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)dms_get_mode_ready, NULL);
}

static void
dms_setup(void)
{
	M.powered = TRUE;
	ofono_modem_changed("Powered");
	qmi_client_dms_get_manufacturer(DMS, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)dms_str_ready, "Manufacturer");
	qmi_client_dms_get_model(DMS, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)dms_str_ready, "Model");
	qmi_client_dms_get_revision(DMS, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)dms_str_ready, "Revision");
	qmi_client_dms_get_ids(DMS, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)dms_str_ready, "Serial");
	dms_get_mode();
}

/* the SIM's IMSI and ICCID (DMS's UIM messages), once it is ready */
static void
dms_imsi_ready(QmiClientDms *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	const char *s = NULL;
	QmiMessageDmsUimGetImsiOutput *o = qmi_client_dms_uim_get_imsi_finish(c, res, &error);

	(void)data;
	if (o && qmi_message_dms_uim_get_imsi_output_get_result(o, &error) &&
	    qmi_message_dms_uim_get_imsi_output_get_imsi(o, &s, NULL)) {
		set_str(&M.imsi, s);
		mlog("sim: IMSI read");
		ofono_sim_changed("SubscriberIdentity");
	} else {
		fail(NULL, "sim: IMSI", error);
	}
	if (o)
		qmi_message_dms_uim_get_imsi_output_unref(o);
}

static void
dms_iccid_ready(QmiClientDms *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	const char *s = NULL;
	QmiMessageDmsUimGetIccidOutput *o = qmi_client_dms_uim_get_iccid_finish(c, res, &error);

	(void)data;
	if (o && qmi_message_dms_uim_get_iccid_output_get_result(o, &error) &&
	    qmi_message_dms_uim_get_iccid_output_get_iccid(o, &s, NULL)) {
		set_str(&M.iccid, s);
		mlog("sim: ICCID read");
		ofono_sim_changed("CardIdentifier");
	} else {
		fail(NULL, "sim: ICCID", error);
	}
	if (o)
		qmi_message_dms_uim_get_iccid_output_unref(o);
}

/* --- UIM: the card and its PIN ------------------------------------------ */

struct pin_ctx {
	GDBusMethodInvocation *inv;
	gboolean automatic;	/* from the PIN file */
	gboolean unblock;
};

static gboolean upin;	/* the card's UPIN replaces PIN1 */

static void
pin_ready(QmiClientUim *c, GAsyncResult *res, struct pin_ctx *x)
{
	GError *error = NULL;
	guint8 left = 0, unblock_left = 0;
	gboolean have_left = FALSE, ok;

	if (x->unblock) {
		QmiMessageUimUnblockPinOutput *o = qmi_client_uim_unblock_pin_finish(c, res, &error);

		ok = o && qmi_message_uim_unblock_pin_output_get_result(o, &error);
		if (o) {
			have_left = qmi_message_uim_unblock_pin_output_get_retries_remaining(o,
			    &left, &unblock_left, NULL);
			qmi_message_uim_unblock_pin_output_unref(o);
		}
	} else {
		QmiMessageUimVerifyPinOutput *o = qmi_client_uim_verify_pin_finish(c, res, &error);

		ok = o && qmi_message_uim_verify_pin_output_get_result(o, &error);
		if (o) {
			have_left = qmi_message_uim_verify_pin_output_get_retries_remaining(o,
			    &left, &unblock_left, NULL);
			qmi_message_uim_verify_pin_output_unref(o);
		}
	}
	pin_busy = FALSE;
	if (have_left) {
		M.pin_retries = left;
		M.puk_retries = unblock_left;
		ofono_sim_changed("Retries");
	}
	if (ok) {
		mlog("sim: %s%s accepted", x->unblock ? "PUK" : "PIN",
		    x->automatic ? " (PIN file)" : "");
		if (x->inv)
			g_dbus_method_invocation_return_value(x->inv, NULL);
		g_clear_error(&error);
	} else {
		if (have_left)
			mlog("sim: %s refused, %u tries left (PUK: %u)",
			    x->unblock ? "PUK" : "PIN", left, unblock_left);
		fail(x->inv, x->unblock ? "sim: PUK" : "sim: PIN", error);
	}
	/* the card's state either way (also comes as its indication) */
	uim_card_status();
	g_free(x);
}

/* the session: the primary GW provisioning application, no AID */
static GArray *
no_aid(void)
{
	return g_array_new(FALSE, FALSE, sizeof(guint8));
}

static void
verify_pin(const char *pin, struct pin_ctx *x)
{
	QmiMessageUimVerifyPinInput *in = qmi_message_uim_verify_pin_input_new();
	GArray *aid = no_aid();

	qmi_message_uim_verify_pin_input_set_session(in,
	    QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING, aid, NULL);
	qmi_message_uim_verify_pin_input_set_info(in,
	    upin ? QMI_UIM_PIN_ID_UPIN : QMI_UIM_PIN_ID_PIN1, pin, NULL);
	pin_busy = TRUE;
	qmi_client_uim_verify_pin(UIM, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)pin_ready, x);
	qmi_message_uim_verify_pin_input_unref(in);
	g_array_unref(aid);
}

static gboolean
pin_valid(const char *pin, size_t min, size_t max)
{
	size_t n = strlen(pin);

	if (n < min || n > max)
		return FALSE;
	for (size_t i = 0; i < n; i++)
		if (pin[i] < '0' || pin[i] > '9')
			return FALSE;
	return TRUE;
}

void
qmi_enter_pin(const char *type, const char *pin, GDBusMethodInvocation *inv)
{
	struct pin_ctx *x;

	if (cl_uim == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_AVAILABLE, "no UIM");
		return;
	}
	if (g_strcmp0(type, "pin") != 0) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, type);
		return;
	}
	if (!pin_valid(pin, 4, 8)) {
		ofono_reply_error(inv, OFONO_ERROR_INVALID_FORMAT, "4 to 8 digits");
		return;
	}
	if (pin_busy) {
		ofono_reply_error(inv, OFONO_ERROR_IN_PROGRESS, "a PIN is being checked");
		return;
	}
	x = g_new0(struct pin_ctx, 1);
	x->inv = inv;
	mlog("sim: PIN entered over D-Bus");
	verify_pin(pin, x);
}

void
qmi_reset_pin(const char *type, const char *puk, const char *pin,
    GDBusMethodInvocation *inv)
{
	QmiMessageUimUnblockPinInput *in;
	struct pin_ctx *x;
	GArray *aid;

	if (cl_uim == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_AVAILABLE, "no UIM");
		return;
	}
	if (g_strcmp0(type, "puk") != 0) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_IMPLEMENTED, type);
		return;
	}
	if (!pin_valid(puk, 8, 8) || !pin_valid(pin, 4, 8)) {
		ofono_reply_error(inv, OFONO_ERROR_INVALID_FORMAT, "PUK 8, PIN 4 to 8 digits");
		return;
	}
	if (pin_busy) {
		ofono_reply_error(inv, OFONO_ERROR_IN_PROGRESS, "a PIN is being checked");
		return;
	}
	x = g_new0(struct pin_ctx, 1);
	x->inv = inv;
	x->unblock = TRUE;
	in = qmi_message_uim_unblock_pin_input_new();
	aid = no_aid();
	qmi_message_uim_unblock_pin_input_set_session(in,
	    QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING, aid, NULL);
	qmi_message_uim_unblock_pin_input_set_info(in,
	    upin ? QMI_UIM_PIN_ID_UPIN : QMI_UIM_PIN_ID_PIN1, puk, pin, NULL);
	pin_busy = TRUE;
	mlog("sim: PUK entered over D-Bus");
	qmi_client_uim_unblock_pin(UIM, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)pin_ready, x);
	qmi_message_uim_unblock_pin_input_unref(in);
	g_array_unref(aid);
}

/*
 * The PIN file (opt.pin_file, the user's, on userdata): tried once per
 * boot (opt.pin_tried, in /var/run, which starts empty), and only while the
 * card has at least opt.pin_min_retries tries left, so that a wrong PIN in
 * the file cannot use up the tries over unattended reboots. The PIN is
 * never logged.
 */
static void
pin_auto(void)
{
	char buf[32];
	struct stat st;
	ssize_t n;
	int fd;

	if (opt.pin_file == NULL || pin_busy)
		return;
	if (access(opt.pin_tried, F_OK) == 0) {
		mlog("sim: PIN required; the PIN file was tried in this boot already");
		return;
	}
	if (M.pin_retries >= 0 && M.pin_retries < opt.pin_min_retries) {
		mlog("sim: PIN required; %d tries left, fewer than %d: no attempt from the PIN file",
		    M.pin_retries, opt.pin_min_retries);
		return;
	}
	fd = open(opt.pin_file, O_RDONLY);
	if (fd < 0) {
		mlog("sim: PIN required; no PIN file (%s: %s)", opt.pin_file, strerror(errno));
		return;
	}
	if (fstat(fd, &st) == 0 && (st.st_mode & 077))
		mlog("sim: %s is readable by others (mode %03o)", opt.pin_file,
		    (unsigned)(st.st_mode & 0777));
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	buf[strcspn(buf, " \t\r\n")] = 0;
	if (!pin_valid(buf, 4, 8)) {
		mlog("sim: the PIN file does not hold 4 to 8 digits: not used");
		explicit_bzero(buf, sizeof(buf));
		return;
	}
	/* marked before the attempt: a crash during it counts as one */
	fd = open(opt.pin_tried, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		mlog("sim: cannot mark the attempt (%s: %s): no attempt", opt.pin_tried,
		    strerror(errno));
		explicit_bzero(buf, sizeof(buf));
		return;
	}
	close(fd);
	mlog("sim: PIN from the PIN file (%d tries left)", M.pin_retries);
	struct pin_ctx *x = g_new0(struct pin_ctx, 1);
	x->automatic = TRUE;
	verify_pin(buf, x);
	explicit_bzero(buf, sizeof(buf));
}

/* one card's state, as the get and the indication give it */
struct card {
	gboolean present;
	QmiUimPinState upin_state;
	guint8 upin_retries, upuk_retries;
	gboolean have_app;
	QmiUimCardApplicationState app_state;
	gboolean upin_replaces_pin1;
	QmiUimPinState pin1_state;
	guint8 pin1_retries, puk1_retries;
};

static void
card_update(const struct card *k)
{
	const char *req = "none";
	gboolean was_ready = M.sim_present && !g_strcmp0(M.pin_required, "none");
	QmiUimPinState st;
	int left, puk;

	if (M.sim_present != k->present) {
		M.sim_present = k->present;
		mlog("sim: %s", k->present ? "present" : "absent");
		ofono_sim_changed("Present");
	}
	upin = k->upin_replaces_pin1;
	st = upin ? k->upin_state : k->pin1_state;
	left = upin ? k->upin_retries : k->pin1_retries;
	puk = upin ? k->upuk_retries : k->puk1_retries;
	if (!k->present)
		req = "none";
	else if (st == QMI_UIM_PIN_STATE_ENABLED_NOT_VERIFIED ||
	    k->app_state == QMI_UIM_CARD_APPLICATION_STATE_PIN1_OR_UPIN_PIN_REQUIRED)
		req = "pin";
	else if (st == QMI_UIM_PIN_STATE_BLOCKED ||
	    st == QMI_UIM_PIN_STATE_PERMANENTLY_BLOCKED ||
	    k->app_state == QMI_UIM_CARD_APPLICATION_STATE_PUK1_OR_UPIN_PUK_REQUIRED ||
	    k->app_state == QMI_UIM_CARD_APPLICATION_STATE_PIN1_BLOCKED)
		req = "puk";
	if (k->present && k->have_app && (M.pin_retries != left || M.puk_retries != puk)) {
		M.pin_retries = left;
		M.puk_retries = puk;
		ofono_sim_changed("Retries");
	}
	if (g_strcmp0(M.pin_required, req) != 0) {
		M.pin_required = req;
		mlog("sim: PIN required: %s (tries %d, PUK %d)%s", req, M.pin_retries,
		    M.puk_retries, k->present && k->have_app ?
		    "" : " (no application yet)");
		ofono_sim_changed("PinRequired");
		ofono_sim_changed("LockedPins");
	}
	if (k->present && !strcmp(req, "pin"))
		pin_auto();
	else if (k->present && !strcmp(req, "puk"))
		mlog("sim: the PIN is blocked; the PUK is needed (SimManager.ResetPin)");
	if (was_ready != (M.sim_present && !strcmp(M.pin_required, "none")))
		sim_ready_changed();
}

/*
 * The card of the primary GW session (its index: slot in the high byte,
 * application in the low one; 0xffff: none, then the first card with a
 * SIM or USIM application).
 */
#define	CARDS_TO_CARD(ElemT, AppT, cards, gw, k) do {			\
	guint ci = 0, ai = 0;						\
	gboolean idx = (gw) != 0xffff;					\
	if (idx) { ci = (gw) >> 8; ai = (gw) & 0xff; }			\
	memset((k), 0, sizeof(*(k)));					\
	if ((cards) == NULL || ci >= (cards)->len)			\
		break;							\
	ElemT *e = &g_array_index((cards), ElemT, ci);			\
	(k)->present = e->card_state == QMI_UIM_CARD_STATE_PRESENT;	\
	(k)->upin_state = e->upin_state;				\
	(k)->upin_retries = e->upin_retries;				\
	(k)->upuk_retries = e->upuk_retries;				\
	if (e->applications == NULL)					\
		break;							\
	for (guint i = 0; i < e->applications->len; i++) {		\
		AppT *a = &g_array_index(e->applications, AppT, i);	\
		if (idx ? i != ai : (a->type != QMI_UIM_CARD_APPLICATION_TYPE_SIM && \
		    a->type != QMI_UIM_CARD_APPLICATION_TYPE_USIM))	\
			continue;					\
		(k)->have_app = TRUE;					\
		(k)->app_state = a->state;				\
		(k)->upin_replaces_pin1 = a->upin_replaces_pin1;	\
		(k)->pin1_state = a->pin1_state;			\
		(k)->pin1_retries = a->pin1_retries;			\
		(k)->puk1_retries = a->puk1_retries;			\
		break;							\
	}								\
} while (0)

static void
uim_status_ready(QmiClientUim *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	guint16 gw = 0xffff, x1, gw2, x2;
	GArray *cards = NULL;
	struct card k;
	QmiMessageUimGetCardStatusOutput *o = qmi_client_uim_get_card_status_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_uim_get_card_status_output_get_result(o, &error) ||
	    !qmi_message_uim_get_card_status_output_get_card_status(o, &gw, &x1, &gw2, &x2,
	    &cards, &error)) {
		fail(NULL, "sim: card status", error);
	} else {
		CARDS_TO_CARD(QmiMessageUimGetCardStatusOutputCardStatusCardsElement,
		    QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2,
		    cards, gw, &k);
		card_update(&k);
	}
	if (o)
		qmi_message_uim_get_card_status_output_unref(o);
}

static void
uim_card_status(void)
{
	qmi_client_uim_get_card_status(UIM, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)uim_status_ready, NULL);
}

static void
uim_status_ind(QmiClientUim *c, QmiIndicationUimCardStatusOutput *o, gpointer data)
{
	guint16 gw = 0xffff, x1, gw2, x2;
	GArray *cards = NULL;
	struct card k;

	(void)c; (void)data;
	if (!qmi_indication_uim_card_status_output_get_card_status(o, &gw, &x1, &gw2, &x2,
	    &cards, NULL))
		return;
	mdbg("sim: card status indication");
	CARDS_TO_CARD(QmiIndicationUimCardStatusOutputCardStatusCardsElement,
	    QmiIndicationUimCardStatusOutputCardStatusCardsElementApplicationsElementV2,
	    cards, gw, &k);
	card_update(&k);
}

static void
uim_register_ready(QmiClientUim *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	QmiMessageUimRegisterEventsOutput *o = qmi_client_uim_register_events_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_uim_register_events_output_get_result(o, &error))
		fail(NULL, "sim: register for card events", error);
	if (o)
		qmi_message_uim_register_events_output_unref(o);
	uim_card_status();
}

static void
uim_setup(void)
{
	QmiMessageUimRegisterEventsInput *in = qmi_message_uim_register_events_input_new();

	g_signal_connect(cl_uim, "card-status", G_CALLBACK(uim_status_ind), NULL);
	qmi_message_uim_register_events_input_set_event_registration_mask(in,
	    QMI_UIM_EVENT_REGISTRATION_FLAG_CARD_STATUS, NULL);
	qmi_client_uim_register_events(UIM, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)uim_register_ready, NULL);
	qmi_message_uim_register_events_input_unref(in);
}

/* --- NAS ---------------------------------------------------------------- */

static void
netreg_set(const char **field, const char *v, const char *prop)
{
	if (g_strcmp0(*field, v) == 0)
		return;
	*field = v;
	ofono_netreg_changed(prop);
}

static void
netreg_str(char **field, const char *v, const char *prop)
{
	if (g_strcmp0(*field, v) == 0)
		return;
	set_str(field, v);
	ofono_netreg_changed(prop);
}

/* the serving system, from the get's response or its indication */
struct serving {
	gboolean have;
	QmiNasRegistrationState reg;
	GArray *radios;		/* QmiNasRadioInterface (gint8) */
	gboolean have_roaming;
	QmiNasRoamingIndicatorStatus roaming;
	gboolean have_plmn;
	guint16 mcc, mnc;
	const char *desc;
	gboolean mnc3;		/* MNC PCS digit (three digits) */
	gboolean have_lac, have_cid, have_tac;
	guint16 lac, tac;
	guint32 cid;
};

static void
serving_update(const struct serving *s)
{
	const char *st, *tech = NULL;
	char buf[8];

	switch (s->reg) {
	case QMI_NAS_REGISTRATION_STATE_REGISTERED:
		st = s->have_roaming && s->roaming == QMI_NAS_ROAMING_INDICATOR_STATUS_ON ?
		    "roaming" : "registered";
		break;
	case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED:
		st = "unregistered";
		break;
	case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED_SEARCHING:
		st = "searching";
		break;
	case QMI_NAS_REGISTRATION_STATE_REGISTRATION_DENIED:
		st = "denied";
		break;
	default:
		st = "unknown";
		break;
	}
	for (guint i = 0; s->radios != NULL && i < s->radios->len; i++) {
		gint8 r = g_array_index(s->radios, gint8, i);

		/* the best one the modem lists */
		if (r == QMI_NAS_RADIO_INTERFACE_LTE)
			tech = "lte";
		else if (r == QMI_NAS_RADIO_INTERFACE_UMTS && g_strcmp0(tech, "lte"))
			tech = "umts";
		else if (r == QMI_NAS_RADIO_INTERFACE_GSM && tech == NULL)
			tech = "gsm";
	}
	if (g_strcmp0(M.reg_status, st) != 0)
		mlog("nas: %s%s%s", st, tech ? " on " : "", tech ? tech : "");
	netreg_set(&M.reg_status, st, "Status");
	netreg_set(&M.technology, tech, "Technology");
	if (s->have_plmn && s->reg == QMI_NAS_REGISTRATION_STATE_REGISTERED) {
		g_snprintf(buf, sizeof(buf), "%03u", s->mcc);
		netreg_str(&M.op_mcc, buf, "MobileCountryCode");
		g_snprintf(buf, sizeof(buf), s->mnc3 || s->mnc > 99 ? "%03u" : "%02u", s->mnc);
		netreg_str(&M.op_mnc, buf, "MobileNetworkCode");
		if (s->desc != NULL && s->desc[0] && g_strcmp0(M.op_name, s->desc)) {
			mlog("nas: operator %s (%s %s)", s->desc, M.op_mcc, M.op_mnc);
			netreg_str(&M.op_name, s->desc, "Name");
		} else if ((s->desc == NULL || !s->desc[0]) && M.op_name == NULL) {
			char *n = g_strdup_printf("%s %s", M.op_mcc, M.op_mnc);

			netreg_str(&M.op_name, n, "Name");
			g_free(n);
		}
	}
	/* the location area: the TAC on LTE (as oFono's QMI driver) */
	if ((s->have_tac && !g_strcmp0(tech, "lte")) || s->have_lac) {
		guint16 lac = s->have_tac && !g_strcmp0(tech, "lte") ? s->tac : s->lac;

		if (!M.have_lac || M.lac != lac) {
			M.have_lac = TRUE;
			M.lac = lac;
			ofono_netreg_changed("LocationAreaCode");
		}
	}
	if (s->have_cid && (!M.have_cell || M.cell_id != s->cid)) {
		M.have_cell = TRUE;
		M.cell_id = s->cid;
		ofono_netreg_changed("CellId");
	}
}

#define	SERVING_FROM(pfx, o, s) do {						\
	QmiNasAttachState cs, ps;						\
	QmiNasNetworkType net;							\
	guint16 m1, m2;								\
	memset((s), 0, sizeof(*(s)));						\
	(s)->have = pfx##_get_serving_system((o), &(s)->reg, &cs, &ps, &net,	\
	    &(s)->radios, NULL);						\
	(s)->have_roaming = pfx##_get_roaming_indicator((o), &(s)->roaming, NULL); \
	(s)->have_plmn = pfx##_get_current_plmn((o), &(s)->mcc, &(s)->mnc,	\
	    &(s)->desc, NULL);							\
	pfx##_get_mnc_pcs_digit_include_status((o), &m1, &m2, &(s)->mnc3, NULL); \
	(s)->have_lac = pfx##_get_lac_3gpp((o), &(s)->lac, NULL);		\
	(s)->have_cid = pfx##_get_cid_3gpp((o), &(s)->cid, NULL);		\
	(s)->have_tac = pfx##_get_lte_tac((o), &(s)->tac, NULL);		\
} while (0)

static void
nas_serving_ready(QmiClientNas *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	struct serving s;
	QmiMessageNasGetServingSystemOutput *o = qmi_client_nas_get_serving_system_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_nas_get_serving_system_output_get_result(o, &error)) {
		fail(NULL, "nas: serving system", error);
	} else {
		SERVING_FROM(qmi_message_nas_get_serving_system_output, o, &s);
		if (s.have)
			serving_update(&s);
	}
	if (o)
		qmi_message_nas_get_serving_system_output_unref(o);
}

static void
nas_serving_ind(QmiClientNas *c, QmiIndicationNasServingSystemOutput *o, gpointer data)
{
	struct serving s;

	(void)c; (void)data;
	SERVING_FROM(qmi_indication_nas_serving_system_output, o, &s);
	if (s.have)
		serving_update(&s);
}

static void
nas_time_ind(QmiClientNas *c, QmiIndicationNasNetworkTimeOutput *o, gpointer data)
{
	guint16 y;
	guint8 mo, d, h, mi, s;
	gint8 tz = 0;
	QmiNasDayOfWeek dow;
	struct tm tm;

	(void)c; (void)data;
	if (!qmi_indication_nas_network_time_output_get_universal_time(o, &y, &mo, &d, &h,
	    &mi, &s, &dow, NULL))
		return;
	qmi_indication_nas_network_time_output_get_timezone_offset(o, &tz, NULL);
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = s;
	M.net_time = timegm(&tm);
	M.net_tz = tz * 15;	/* quarter hours */
	mlog("nas: network time %04u-%02u-%02u %02u:%02u:%02u UTC, zone %+d min "
	    "(this system: %+ld s)", y, mo, d, h, mi, s, M.net_tz,
	    (long)(time(NULL) - M.net_time));
}

/* 3GPP 27.007 +CSQ's range, -113 to -51 dBm, as 0-100 % */
static int
dbm_percent(int dbm)
{
	int p = (dbm + 113) * 100 / 62;

	return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void
nas_signal_ready(QmiClientNas *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	gint8 dbm;
	QmiNasRadioInterface ri;
	QmiMessageNasGetSignalStrengthOutput *o = qmi_client_nas_get_signal_strength_finish(c, res, &error);

	(void)data;
	if (o && qmi_message_nas_get_signal_strength_output_get_result(o, &error) &&
	    qmi_message_nas_get_signal_strength_output_get_signal_strength(o, &dbm, &ri, NULL)) {
		int p = dbm_percent(dbm);

		if (p != M.strength) {
			mdbg("nas: signal %d dBm (%s): %d %%", dbm,
			    qmi_nas_radio_interface_get_string(ri), p);
			M.strength = p;
			ofono_netreg_changed("Strength");
		}
	} else if (error != NULL) {
		/* not registered: no signal to report */
		mdbg("nas: signal: %s", error->message);
		g_clear_error(&error);
		if (M.strength != -1) {
			M.strength = -1;
		}
	}
	if (o)
		qmi_message_nas_get_signal_strength_output_unref(o);
}

static gboolean
nas_poll(gpointer data)
{
	(void)data;
	if (cl_nas != NULL && M.online)
		qmi_client_nas_get_signal_strength(NAS, NULL, TIMEOUT, NULL,
		    (GAsyncReadyCallback)nas_signal_ready, NULL);
	return G_SOURCE_CONTINUE;
}

static void
nas_register_ready(QmiClientNas *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	QmiMessageNasRegisterIndicationsOutput *o = qmi_client_nas_register_indications_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_nas_register_indications_output_get_result(o, &error))
		fail(NULL, "nas: register indications", error);
	if (o)
		qmi_message_nas_register_indications_output_unref(o);
	qmi_client_nas_get_serving_system(NAS, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)nas_serving_ready, NULL);
	nas_poll(NULL);
}

static void
nas_setup(void)
{
	QmiMessageNasRegisterIndicationsInput *in = qmi_message_nas_register_indications_input_new();

	g_signal_connect(cl_nas, "serving-system", G_CALLBACK(nas_serving_ind), NULL);
	g_signal_connect(cl_nas, "network-time", G_CALLBACK(nas_time_ind), NULL);
	qmi_message_nas_register_indications_input_set_serving_system_events(in, TRUE, NULL);
	qmi_message_nas_register_indications_input_set_network_time(in, TRUE, NULL);
	qmi_client_nas_register_indications(NAS, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)nas_register_ready, NULL);
	qmi_message_nas_register_indications_input_unref(in);
	if (opt.signal_poll > 0)
		signal_timer = g_timeout_add_seconds(opt.signal_poll, nas_poll, NULL);
	M.reg_status = "unregistered";
	M.strength = -1;
	ofono_interfaces_changed();
}

void
qmi_register(GDBusMethodInvocation *inv)
{
	/* the modem registers by itself (automatic selection) */
	mlog("nas: Register: automatic selection, nothing to do");
	g_dbus_method_invocation_return_value(inv, NULL);
}

/* --- WMS: messages ---------------------------------------------------- */

/* a concatenated message's parts until all have come */
struct concat {
	char *sender;
	guint16 ref;
	guint8 total, have;
	char **parts;		/* [total] */
	time_t first;
	int tz;
	gboolean class0;
};

static GList *concats;

static void
concat_emit(struct concat *k)
{
	GString *s = g_string_new(NULL);

	for (guint i = 0; i < k->total; i++)
		g_string_append(s, k->parts[i] ? k->parts[i] : "");
	if (k->have < k->total)
		mlog("sms: %u of %u parts of a message from %s came: delivered so",
		    k->have, k->total, k->sender);
	ofono_incoming_message(s->str, k->sender, k->first, k->tz, k->class0);
	g_string_free(s, TRUE);
	concats = g_list_remove(concats, k);
	g_strfreev(k->parts);
	g_free(k->sender);
	g_free(k);
}

static gboolean
concat_expire(gpointer data)
{
	time_t now = time(NULL);

	(void)data;
	for (GList *l = concats; l != NULL; ) {
		struct concat *k = l->data;

		l = l->next;
		if (now - k->first > CONCAT_TIMEOUT)
			concat_emit(k);
	}
	return G_SOURCE_CONTINUE;
}

static void
sms_deliver(const guint8 *pdu, guint len)
{
	struct sms_part p;
	struct concat *k = NULL;

	if (sms_decode(pdu, len, &p) != 0) {
		mlog("sms: a PDU of %u bytes that is no message (status report?): skipped", len);
		return;
	}
	if (p.submit) {
		mdbg("sms: a stored SMS-SUBMIT: skipped");
		return;
	}
	if (p.total < 2) {
		ofono_incoming_message(p.text, p.number, p.time, p.tz_minutes, sms_class0(&p));
		return;
	}
	for (GList *l = concats; l != NULL; l = l->next) {
		struct concat *c = l->data;

		if (c->ref == p.ref && c->total == p.total && !strcmp(c->sender, p.number)) {
			k = c;
			break;
		}
	}
	if (k == NULL) {
		k = g_new0(struct concat, 1);
		k->sender = g_strdup(p.number);
		k->ref = p.ref;
		k->total = p.total;
		k->parts = g_new0(char *, p.total + 1);
		k->first = p.time ? p.time : time(NULL);
		k->tz = p.tz_minutes;
		k->class0 = sms_class0(&p);
		concats = g_list_append(concats, k);
	}
	if (k->parts[p.seq - 1] == NULL) {
		k->parts[p.seq - 1] = g_strdup(p.text);
		k->have++;
	}
	mdbg("sms: part %u of %u (ref %u) from %s", p.seq, p.total, p.ref, p.number);
	if (k->have == k->total)
		concat_emit(k);
}

struct stored {
	QmiWmsStorageType storage;
	guint32 index;
};

static void
wms_tag_ready(QmiClientWms *c, GAsyncResult *res, struct stored *s)
{
	GError *error = NULL;
	QmiMessageWmsModifyTagOutput *o = qmi_client_wms_modify_tag_finish(c, res, &error);

	if (o == NULL || !qmi_message_wms_modify_tag_output_get_result(o, &error))
		fail(NULL, "sms: tag read", error);
	if (o)
		qmi_message_wms_modify_tag_output_unref(o);
	g_free(s);
}

static void
wms_read_ready(QmiClientWms *c, GAsyncResult *res, struct stored *s)
{
	GError *error = NULL;
	QmiWmsMessageTagType tag;
	QmiWmsMessageFormat format;
	GArray *raw = NULL;
	QmiMessageWmsRawReadOutput *o = qmi_client_wms_raw_read_finish(c, res, &error);

	if (o == NULL || !qmi_message_wms_raw_read_output_get_result(o, &error) ||
	    !qmi_message_wms_raw_read_output_get_raw_message_data(o, &tag, &format, &raw, &error)) {
		fail(NULL, "sms: read", error);
		g_free(s);
	} else {
		mdbg("sms: %s %u: %s, %u bytes", qmi_wms_storage_type_get_string(s->storage),
		    s->index, qmi_wms_message_tag_type_get_string(tag), raw->len);
		if (format == QMI_WMS_MESSAGE_FORMAT_GSM_WCDMA_POINT_TO_POINT)
			sms_deliver((const guint8 *)raw->data, raw->len);
		else
			mlog("sms: format %s: skipped", qmi_wms_message_format_get_string(format));
		if (tag == QMI_WMS_MESSAGE_TAG_TYPE_MT_NOT_READ) {
			QmiMessageWmsModifyTagInput *in = qmi_message_wms_modify_tag_input_new();

			qmi_message_wms_modify_tag_input_set_message_tag(in, s->storage,
			    s->index, QMI_WMS_MESSAGE_TAG_TYPE_MT_READ, NULL);
			qmi_message_wms_modify_tag_input_set_message_mode(in,
			    QMI_WMS_MESSAGE_MODE_GSM_WCDMA, NULL);
			qmi_client_wms_modify_tag(WMS, in, TIMEOUT, NULL,
			    (GAsyncReadyCallback)wms_tag_ready, s);
			qmi_message_wms_modify_tag_input_unref(in);
		} else {
			g_free(s);
		}
	}
	if (o)
		qmi_message_wms_raw_read_output_unref(o);
}

static void
wms_read(QmiWmsStorageType storage, guint32 index)
{
	QmiMessageWmsRawReadInput *in = qmi_message_wms_raw_read_input_new();
	struct stored *s = g_new0(struct stored, 1);

	s->storage = storage;
	s->index = index;
	qmi_message_wms_raw_read_input_set_message_memory_storage_id(in, storage, index, NULL);
	qmi_message_wms_raw_read_input_set_message_mode(in, QMI_WMS_MESSAGE_MODE_GSM_WCDMA, NULL);
	qmi_client_wms_raw_read(WMS, in, TIMEOUT, NULL, (GAsyncReadyCallback)wms_read_ready, s);
	qmi_message_wms_raw_read_input_unref(in);
}

static void
wms_ack_ready(QmiClientWms *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	QmiMessageWmsSendAckOutput *o = qmi_client_wms_send_ack_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_wms_send_ack_output_get_result(o, &error))
		fail(NULL, "sms: ack", error);
	if (o)
		qmi_message_wms_send_ack_output_unref(o);
}

static void
wms_event_ind(QmiClientWms *c, QmiIndicationWmsEventReportOutput *o, gpointer data)
{
	QmiWmsStorageType storage;
	guint32 index, txn;
	QmiWmsAckIndicator ack;
	QmiWmsMessageFormat format;
	GArray *raw = NULL;

	(void)c; (void)data;
	if (qmi_indication_wms_event_report_output_get_mt_message(o, &storage, &index, NULL)) {
		mlog("sms: new message, %s %u", qmi_wms_storage_type_get_string(storage), index);
		wms_read(storage, index);
	}
	/* a route that transfers instead of storing (not ours, but the modem's
	 * own configuration may have one): the message itself, acknowledged */
	if (qmi_indication_wms_event_report_output_get_transfer_route_mt_message(o, &ack, &txn,
	    &format, &raw, NULL)) {
		mlog("sms: message transferred, %u bytes", raw->len);
		if (format == QMI_WMS_MESSAGE_FORMAT_GSM_WCDMA_POINT_TO_POINT)
			sms_deliver((const guint8 *)raw->data, raw->len);
		if (ack == QMI_WMS_ACK_INDICATOR_SEND) {
			QmiMessageWmsSendAckInput *in = qmi_message_wms_send_ack_input_new();

			qmi_message_wms_send_ack_input_set_information(in, txn,
			    QMI_WMS_MESSAGE_PROTOCOL_WCDMA, TRUE, NULL);
			qmi_client_wms_send_ack(WMS, in, TIMEOUT, NULL,
			    (GAsyncReadyCallback)wms_ack_ready, NULL);
			qmi_message_wms_send_ack_input_unref(in);
		}
	}
}

static void
wms_list_ready(QmiClientWms *c, GAsyncResult *res, gpointer storage_p)
{
	GError *error = NULL;
	GArray *list = NULL;
	QmiWmsStorageType storage = GPOINTER_TO_UINT(storage_p);
	QmiMessageWmsListMessagesOutput *o = qmi_client_wms_list_messages_finish(c, res, &error);

	if (o == NULL || !qmi_message_wms_list_messages_output_get_result(o, &error) ||
	    !qmi_message_wms_list_messages_output_get_message_list(o, &list, &error)) {
		/* an empty SIM storage answers with an error on some modems */
		mlog("sms: list %s: %s", qmi_wms_storage_type_get_string(storage),
		    error ? error->message : "failed");
		g_clear_error(&error);
	} else {
		mlog("sms: %u unread in %s", list->len, qmi_wms_storage_type_get_string(storage));
		for (guint i = 0; i < list->len; i++) {
			QmiMessageWmsListMessagesOutputMessageListElement *e =
			    &g_array_index(list, QmiMessageWmsListMessagesOutputMessageListElement, i);

			wms_read(storage, e->memory_index);
		}
	}
	if (o)
		qmi_message_wms_list_messages_output_unref(o);
}

static void
wms_list(QmiWmsStorageType storage)
{
	QmiMessageWmsListMessagesInput *in = qmi_message_wms_list_messages_input_new();

	qmi_message_wms_list_messages_input_set_storage_type(in, storage, NULL);
	qmi_message_wms_list_messages_input_set_message_tag(in,
	    QMI_WMS_MESSAGE_TAG_TYPE_MT_NOT_READ, NULL);
	qmi_message_wms_list_messages_input_set_message_mode(in,
	    QMI_WMS_MESSAGE_MODE_GSM_WCDMA, NULL);
	qmi_client_wms_list_messages(WMS, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)wms_list_ready, GUINT_TO_POINTER(storage));
	qmi_message_wms_list_messages_input_unref(in);
}

static void
wms_simple_ready(GObject *c, GAsyncResult *res, gpointer what)
{
	GError *error = NULL;
	QmiMessageWmsSetRoutesOutput *r = NULL;
	QmiMessageWmsSetEventReportOutput *e = NULL;

	if (!strcmp(what, "routes")) {
		r = qmi_client_wms_set_routes_finish(QMI_CLIENT_WMS(c), res, &error);
		if (r == NULL || !qmi_message_wms_set_routes_output_get_result(r, &error))
			fail(NULL, "sms: set routes", error);
		if (r)
			qmi_message_wms_set_routes_output_unref(r);
	} else {
		e = qmi_client_wms_set_event_report_finish(QMI_CLIENT_WMS(c), res, &error);
		if (e == NULL || !qmi_message_wms_set_event_report_output_get_result(e, &error))
			fail(NULL, "sms: event report", error);
		if (e)
			qmi_message_wms_set_event_report_output_unref(e);
		/* then the messages that came while nobody listened */
		wms_list(QMI_WMS_STORAGE_TYPE_NV);
		wms_list(QMI_WMS_STORAGE_TYPE_UIM);
	}
}

static gboolean wms_done;

static void
wms_setup(void)
{
	QmiMessageWmsSetRoutesInput *rin;
	QmiMessageWmsSetEventReportInput *ein;
	GArray *routes;

	if (wms_done)
		return;
	wms_done = TRUE;
	g_signal_connect(cl_wms, "event-report", G_CALLBACK(wms_event_ind), NULL);
	routes = g_array_new(FALSE, FALSE, sizeof(QmiMessageWmsSetRoutesInputRouteListElement));
	for (int cls = QMI_WMS_MESSAGE_CLASS_0; cls <= QMI_WMS_MESSAGE_CLASS_NONE; cls++) {
		QmiMessageWmsSetRoutesInputRouteListElement e = {
			.message_type = QMI_WMS_MESSAGE_TYPE_POINT_TO_POINT,
			.message_class = cls,
			.storage = cls == QMI_WMS_MESSAGE_CLASS_2 ?
			    QMI_WMS_STORAGE_TYPE_UIM : QMI_WMS_STORAGE_TYPE_NV,
			.receipt_action = QMI_WMS_RECEIPT_ACTION_STORE_AND_NOTIFY,
		};
		g_array_append_val(routes, e);
	}
	rin = qmi_message_wms_set_routes_input_new();
	qmi_message_wms_set_routes_input_set_route_list(rin, routes, NULL);
	qmi_client_wms_set_routes(WMS, rin, TIMEOUT, NULL,
	    (GAsyncReadyCallback)wms_simple_ready, "routes");
	qmi_message_wms_set_routes_input_unref(rin);
	g_array_unref(routes);
	ein = qmi_message_wms_set_event_report_input_new();
	qmi_message_wms_set_event_report_input_set_new_mt_message_indicator(ein, TRUE, NULL);
	qmi_client_wms_set_event_report(WMS, ein, TIMEOUT, NULL,
	    (GAsyncReadyCallback)wms_simple_ready, "event");
	qmi_message_wms_set_event_report_input_unref(ein);
	g_timeout_add_seconds(60, concat_expire, NULL);
}

static void
sim_ready_changed(void)
{
	gboolean ready = M.sim_present && !g_strcmp0(M.pin_required, "none");

	mlog("sim: %s", ready ? "ready" : "not ready");
	ofono_interfaces_changed();
	if (!ready)
		return;
	if (cl_dms != NULL) {
		qmi_client_dms_uim_get_imsi(DMS, NULL, TIMEOUT, NULL,
		    (GAsyncReadyCallback)dms_imsi_ready, NULL);
		qmi_client_dms_uim_get_iccid(DMS, NULL, TIMEOUT, NULL,
		    (GAsyncReadyCallback)dms_iccid_ready, NULL);
	}
	if (cl_wms != NULL)
		wms_setup();
}

struct send_ctx {
	struct message *m;
	char *to;
};

static void
wms_send_ready(QmiClientWms *c, GAsyncResult *res, struct send_ctx *x)
{
	GError *error = NULL;
	guint16 id = 0;
	QmiWmsGsmUmtsRpCause rp;
	QmiWmsGsmUmtsTpCause tp;
	QmiMessageWmsRawSendOutput *o = qmi_client_wms_raw_send_finish(c, res, &error);

	if (o != NULL && qmi_message_wms_raw_send_output_get_result(o, &error)) {
		qmi_message_wms_raw_send_output_get_message_id(o, &id, NULL);
		x->m->sent++;
		mlog("sms: part %d of %d to %s sent (reference %u)", x->m->sent + x->m->failed,
		    x->m->parts, x->to, id);
	} else {
		x->m->failed++;
		if (o != NULL && qmi_message_wms_raw_send_output_get_gsm_wcdma_cause_info(o,
		    &rp, &tp, NULL))
			mlog("sms: part refused: RP cause %s, TP cause %s",
			    qmi_wms_gsm_umts_rp_cause_get_string(rp),
			    qmi_wms_gsm_umts_tp_cause_get_string(tp));
		fail(NULL, "sms: send", error);
	}
	if (o)
		qmi_message_wms_raw_send_output_unref(o);
	if (x->m->sent + x->m->failed == x->m->parts) {
		ofono_message_state(x->m, x->m->failed ? "failed" : "sent");
		g_free(x->to);
		g_free(x);
	}
}

static gboolean
number_valid(const char *n, size_t max)
{
	size_t len = strlen(n), i = n[0] == '+';

	if (len - i < 1 || len - i > max)
		return FALSE;
	for (; i < len; i++)
		if (!g_ascii_isdigit(n[i]) && n[i] != '*' && n[i] != '#')
			return FALSE;
	return TRUE;
}

void
qmi_send_sms(const char *to, const char *text, GDBusMethodInvocation *inv)
{
	static guint8 ref;
	static guint8 pdus[SMS_PARTS_MAX][SMS_PDU_MAX];
	guint32 len[SMS_PARTS_MAX];
	struct send_ctx *x;
	int parts;

	if (cl_wms == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_AVAILABLE, "no WMS");
		return;
	}
	if (!number_valid(to, 20) || !g_utf8_validate(text, -1, NULL)) {
		ofono_reply_error(inv, OFONO_ERROR_INVALID_FORMAT, to);
		return;
	}
	parts = sms_encode(to, text, ++ref, pdus, len, SMS_PARTS_MAX);
	if (parts == 0) {
		ofono_reply_error(inv, OFONO_ERROR_INVALID_FORMAT, "too long");
		return;
	}
	x = g_new0(struct send_ctx, 1);
	x->to = g_strdup(to);
	x->m = ofono_message_new(parts);
	mlog("sms: to %s, %zu bytes in %d part(s), %s", to, strlen(text), parts, x->m->path);
	g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", x->m->path));
	for (int i = 0; i < parts; i++) {
		QmiMessageWmsRawSendInput *in = qmi_message_wms_raw_send_input_new();
		GArray *raw = g_array_sized_new(FALSE, FALSE, 1, len[i]);

		g_array_append_vals(raw, pdus[i], len[i]);
		qmi_message_wms_raw_send_input_set_raw_message_data(in,
		    QMI_WMS_MESSAGE_FORMAT_GSM_WCDMA_POINT_TO_POINT, raw, NULL);
		/* one at a time on the modem's side: the requests queue there */
		qmi_client_wms_raw_send(WMS, in, 60, NULL,
		    (GAsyncReadyCallback)wms_send_ready, x);
		qmi_message_wms_raw_send_input_unref(in);
		g_array_unref(raw);
	}
}

/* --- VOICE: calls ------------------------------------------------------ */

const char *
call_state_name(enum call_state s)
{
	static const char *const names[] = {
		[CALL_ACTIVE] = "active", [CALL_HELD] = "held",
		[CALL_DIALING] = "dialing", [CALL_ALERTING] = "alerting",
		[CALL_INCOMING] = "incoming", [CALL_WAITING] = "waiting",
		[CALL_DISCONNECTED] = "disconnected",
	};

	return names[s];
}

static enum call_state
call_state_of(QmiVoiceCallState s, QmiVoiceCallDirection dir)
{
	switch (s) {
	case QMI_VOICE_CALL_STATE_ORIGINATION:
	case QMI_VOICE_CALL_STATE_CC_IN_PROGRESS:
		return CALL_DIALING;
	case QMI_VOICE_CALL_STATE_ALERTING:
		/* the far end rings (MO); an MT call alerts the user */
		return dir == QMI_VOICE_CALL_DIRECTION_MT ? CALL_INCOMING : CALL_ALERTING;
	case QMI_VOICE_CALL_STATE_INCOMING:
	case QMI_VOICE_CALL_STATE_SETUP:
		return CALL_INCOMING;
	case QMI_VOICE_CALL_STATE_CONVERSATION:
		return CALL_ACTIVE;
	case QMI_VOICE_CALL_STATE_HOLD:
		return CALL_HELD;
	case QMI_VOICE_CALL_STATE_WAITING:
		return CALL_WAITING;
	case QMI_VOICE_CALL_STATE_DISCONNECTING:
	case QMI_VOICE_CALL_STATE_END:
	default:
		return CALL_DISCONNECTED;
	}
}

gboolean
calls_live(void)
{
	for (GList *l = M.calls; l != NULL; l = l->next)
		if (((struct call *)l->data)->state != CALL_DISCONNECTED)
			return TRUE;
	return FALSE;
}

struct call *
call_find_path(const char *path)
{
	for (GList *l = M.calls; l != NULL; l = l->next)
		if (!strcmp(((struct call *)l->data)->path, path))
			return l->data;
	return NULL;
}

static struct call *
call_find(guint8 id)
{
	for (GList *l = M.calls; l != NULL; l = l->next)
		if (((struct call *)l->data)->id == id)
			return l->data;
	return NULL;
}

static struct call *
call_new(guint8 id, const char *number, enum call_state st, gboolean incoming)
{
	struct call *c = g_new0(struct call, 1);

	c->id = id;
	c->n = ++call_counter;
	c->path = g_strdup_printf("%s/voicecall%02u", M.path, c->n);
	c->number = g_strdup(number ? number : "");
	c->state = st;
	c->incoming = incoming;
	M.calls = g_list_append(M.calls, c);
	mlog("call %u (%s): %s %s, %s", c->id, c->path, incoming ? "from" : "to",
	    c->number[0] ? c->number : "(withheld)", call_state_name(st));
	ofono_call_added(c);
	calls_changed();
	return c;
}

static void
call_gone(struct call *c)
{
	mlog("call %u: disconnected", c->id);
	if (c->state != CALL_DISCONNECTED) {
		c->state = CALL_DISCONNECTED;
		ofono_call_changed(c, "State");
	}
	ofono_call_removed(c);
	M.calls = g_list_remove(M.calls, c);
	g_free(c->number);
	g_free(c->path);
	g_free(c);
	calls_changed();
}

static void
call_set_state(struct call *c, enum call_state st)
{
	if (c->state == st)
		return;
	if (st == CALL_DISCONNECTED) {
		call_gone(c);
		return;
	}
	mlog("call %u: %s -> %s", c->id, call_state_name(c->state), call_state_name(st));
	c->state = st;
	if (st == CALL_ACTIVE && !c->start_time[0]) {
		time_t t = time(NULL);
		struct tm tm;

		localtime_r(&t, &tm);
		strftime(c->start_time, sizeof(c->start_time), "%Y-%m-%dT%H:%M:%S%z", &tm);
		ofono_call_changed(c, "StartTime");
	}
	ofono_call_changed(c, "State");
	calls_changed();
}

/* one call of the modem's list (All Call Status, Get All Call Info) */
struct qcall {
	guint8 id;
	QmiVoiceCallState state;
	QmiVoiceCallType type;
	QmiVoiceCallDirection dir;
	const char *number;
};

static void
calls_update(struct qcall *q, guint n)
{
	for (GList *l = M.calls; l != NULL; l = l->next)
		((struct call *)l->data)->seen = FALSE;
	for (guint i = 0; i < n; i++) {
		struct call *c = call_find(q[i].id);
		enum call_state st = call_state_of(q[i].state, q[i].dir);

		mdbg("voice: call %u %s %s %s", q[i].id,
		    qmi_voice_call_state_get_string(q[i].state),
		    qmi_voice_call_direction_get_string(q[i].dir),
		    q[i].number ? q[i].number : "");
		if (c == NULL) {
			if (st == CALL_DISCONNECTED)
				continue;
			c = call_new(q[i].id, q[i].number, st,
			    q[i].dir == QMI_VOICE_CALL_DIRECTION_MT);
		}
		c->seen = TRUE;
		if (q[i].type == QMI_VOICE_CALL_TYPE_EMERGENCY && !c->emergency) {
			c->emergency = TRUE;
			ofono_call_changed(c, "Emergency");
		}
		if (q[i].number && q[i].number[0] && g_strcmp0(c->number, q[i].number)) {
			g_free(c->number);
			c->number = g_strdup(q[i].number);
			ofono_call_changed(c, "LineIdentification");
		}
		call_set_state(c, st);
	}
	/* the modem lists every call it has: one not there is over */
	for (GList *l = M.calls; l != NULL; ) {
		struct call *c = l->data;

		l = l->next;
		if (!c->seen)
			call_gone(c);
	}
}

#define	CALLS_FROM(CallT, NumT, info, nums, q, n) do {			\
	(n) = 0;							\
	for (guint i = 0; (info) != NULL && i < (info)->len && i < 8; i++) { \
		CallT *e = &g_array_index((info), CallT, i);		\
		(q)[n].id = e->id;					\
		(q)[n].state = e->state;				\
		(q)[n].type = e->type;					\
		(q)[n].dir = e->direction;				\
		(q)[n].number = NULL;					\
		for (guint j = 0; (nums) != NULL && j < (nums)->len; j++) { \
			NumT *r = &g_array_index((nums), NumT, j);	\
			if (r->id == e->id &&				\
			    r->presentation_indicator == QMI_VOICE_PRESENTATION_ALLOWED) \
				(q)[n].number = r->type;		\
		}							\
		(n)++;							\
	}								\
} while (0)

static void
voice_status_ind(QmiClientVoice *c, QmiIndicationVoiceAllCallStatusOutput *o, gpointer data)
{
	GArray *info = NULL, *nums = NULL;
	struct qcall q[8];
	guint n;

	(void)c; (void)data;
	if (!qmi_indication_voice_all_call_status_output_get_call_information(o, &info, NULL))
		return;
	qmi_indication_voice_all_call_status_output_get_remote_party_number(o, &nums, NULL);
	CALLS_FROM(QmiIndicationVoiceAllCallStatusOutputCallInformationCall,
	    QmiIndicationVoiceAllCallStatusOutputRemotePartyNumberCall, info, nums, q, n);
	calls_update(q, n);
}

static void
voice_info_ready(QmiClientVoice *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	GArray *info = NULL, *nums = NULL;
	struct qcall q[8];
	guint n;
	QmiMessageVoiceGetAllCallInfoOutput *o = qmi_client_voice_get_all_call_info_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_voice_get_all_call_info_output_get_result(o, &error)) {
		fail(NULL, "voice: calls", error);
	} else {
		qmi_message_voice_get_all_call_info_output_get_call_information(o, &info, NULL);
		qmi_message_voice_get_all_call_info_output_get_remote_party_number(o, &nums, NULL);
		CALLS_FROM(QmiMessageVoiceGetAllCallInfoOutputCallInformationCall,
		    QmiMessageVoiceGetAllCallInfoOutputRemotePartyNumberCall, info, nums, q, n);
		mlog("voice: %u call(s) at the start", n);
		calls_update(q, n);
	}
	if (o)
		qmi_message_voice_get_all_call_info_output_unref(o);
}

static void
voice_register_ready(QmiClientVoice *c, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;
	QmiMessageVoiceIndicationRegisterOutput *o = qmi_client_voice_indication_register_finish(c, res, &error);

	(void)data;
	if (o == NULL || !qmi_message_voice_indication_register_output_get_result(o, &error))
		fail(NULL, "voice: register indications", error);
	if (o)
		qmi_message_voice_indication_register_output_unref(o);
	qmi_client_voice_get_all_call_info(VOICE, NULL, TIMEOUT, NULL,
	    (GAsyncReadyCallback)voice_info_ready, NULL);
}

static void
voice_setup(void)
{
	QmiMessageVoiceIndicationRegisterInput *in = qmi_message_voice_indication_register_input_new();

	g_signal_connect(cl_voice, "all-call-status", G_CALLBACK(voice_status_ind), NULL);
	qmi_message_voice_indication_register_input_set_call_notification_events(in, TRUE, NULL);
	qmi_message_voice_indication_register_input_set_supplementary_service_notification_events(in,
	    TRUE, NULL);
	qmi_client_voice_indication_register(VOICE, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)voice_register_ready, NULL);
	qmi_message_voice_indication_register_input_unref(in);
	ofono_interfaces_changed();
}

struct dial_ctx {
	GDBusMethodInvocation *inv;
	char *number;
};

static gboolean dialing;

static void
voice_dial_ready(QmiClientVoice *cl, GAsyncResult *res, struct dial_ctx *x)
{
	GError *error = NULL;
	guint8 id;
	struct call *c;
	QmiMessageVoiceDialCallOutput *o = qmi_client_voice_dial_call_finish(cl, res, &error);

	dialing = FALSE;
	if (o == NULL || !qmi_message_voice_dial_call_output_get_result(o, &error) ||
	    !qmi_message_voice_dial_call_output_get_call_id(o, &id, &error)) {
		fail(x->inv, "voice: dial", error);
	} else {
		/* the call's first notification may have come before */
		c = call_find(id);
		if (c == NULL)
			c = call_new(id, x->number, CALL_DIALING, FALSE);
		g_dbus_method_invocation_return_value(x->inv, g_variant_new("(o)", c->path));
	}
	if (o)
		qmi_message_voice_dial_call_output_unref(o);
	g_free(x->number);
	g_free(x);
}

void
qmi_dial(const char *number, const char *clir, GDBusMethodInvocation *inv)
{
	QmiMessageVoiceDialCallInput *in;
	struct dial_ctx *x;

	if (cl_voice == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_NOT_AVAILABLE, "no VOICE");
		return;
	}
	/* oFono's format: [+][0-9*#]{1,80} */
	if (!number_valid(number, 80)) {
		ofono_reply_error(inv, OFONO_ERROR_INVALID_FORMAT, number);
		return;
	}
	if (dialing) {
		ofono_reply_error(inv, OFONO_ERROR_IN_PROGRESS, "dialling");
		return;
	}
	/* the CLIR override has no TLV in libqmi's Dial Call: the network's */
	if (clir[0] && strcmp(clir, "default"))
		mlog("voice: hide_callerid \"%s\" not passed on (the network's setting)", clir);
	x = g_new0(struct dial_ctx, 1);
	x->inv = inv;
	x->number = g_strdup(number);
	in = qmi_message_voice_dial_call_input_new();
	qmi_message_voice_dial_call_input_set_calling_number(in, number, NULL);
	dialing = TRUE;
	mlog("voice: dial %s", number);
	qmi_client_voice_dial_call(VOICE, in, 30, NULL, (GAsyncReadyCallback)voice_dial_ready, x);
	qmi_message_voice_dial_call_input_unref(in);
}

static void
voice_answer_ready(QmiClientVoice *cl, GAsyncResult *res, GDBusMethodInvocation *inv)
{
	GError *error = NULL;
	QmiMessageVoiceAnswerCallOutput *o = qmi_client_voice_answer_call_finish(cl, res, &error);

	if (o == NULL || !qmi_message_voice_answer_call_output_get_result(o, &error))
		fail(inv, "voice: answer", error);
	else if (inv)
		g_dbus_method_invocation_return_value(inv, NULL);
	if (o)
		qmi_message_voice_answer_call_output_unref(o);
}

void
qmi_answer(struct call *c, GDBusMethodInvocation *inv)
{
	QmiMessageVoiceAnswerCallInput *in;

	if (c->state != CALL_INCOMING && c->state != CALL_WAITING) {
		ofono_reply_error(inv, OFONO_ERROR_FAILED, "not ringing");
		return;
	}
	in = qmi_message_voice_answer_call_input_new();
	qmi_message_voice_answer_call_input_set_call_id(in, c->id, NULL);
	mlog("voice: answer call %u", c->id);
	qmi_client_voice_answer_call(VOICE, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)voice_answer_ready, inv);
	qmi_message_voice_answer_call_input_unref(in);
}

static void
voice_end_ready(QmiClientVoice *cl, GAsyncResult *res, GDBusMethodInvocation *inv)
{
	GError *error = NULL;
	QmiMessageVoiceEndCallOutput *o = qmi_client_voice_end_call_finish(cl, res, &error);

	if (o == NULL || !qmi_message_voice_end_call_output_get_result(o, &error))
		fail(inv, "voice: end", error);
	else if (inv)
		g_dbus_method_invocation_return_value(inv, NULL);
	if (o)
		qmi_message_voice_end_call_output_unref(o);
}

static void
end_call(struct call *c, GDBusMethodInvocation *inv)
{
	QmiMessageVoiceEndCallInput *in = qmi_message_voice_end_call_input_new();

	qmi_message_voice_end_call_input_set_call_id(in, c->id, NULL);
	mlog("voice: end call %u", c->id);
	qmi_client_voice_end_call(VOICE, in, TIMEOUT, NULL, (GAsyncReadyCallback)voice_end_ready, inv);
	qmi_message_voice_end_call_input_unref(in);
}

void
qmi_hangup(struct call *c, GDBusMethodInvocation *inv)
{
	end_call(c, inv);
}

/* every call but a waiting one (voicecallmanager-api.txt) */
void
qmi_hangup_all(GDBusMethodInvocation *inv)
{
	for (GList *l = M.calls; l != NULL; l = l->next) {
		struct call *c = l->data;

		if (c->state != CALL_WAITING && c->state != CALL_DISCONNECTED)
			end_call(c, NULL);
	}
	g_dbus_method_invocation_return_value(inv, NULL);
}

/* DTMF: each tone started and stopped (continuous DTMF) */
struct tones_ctx {
	GDBusMethodInvocation *inv;
	char *tones;
	guint pos;
	guint8 id;
};

static void tone_next(struct tones_ctx *x);

static void
tones_done(struct tones_ctx *x, GError *error)
{
	if (error != NULL)
		fail(x->inv, "voice: DTMF", error);
	else
		g_dbus_method_invocation_return_value(x->inv, NULL);
	g_free(x->tones);
	g_free(x);
}

static gboolean
tone_gap_over(gpointer data)
{
	tone_next(data);
	return G_SOURCE_REMOVE;
}

static void
tone_stop_ready(QmiClientVoice *cl, GAsyncResult *res, struct tones_ctx *x)
{
	GError *error = NULL;
	QmiMessageVoiceStopContinuousDtmfOutput *o = qmi_client_voice_stop_continuous_dtmf_finish(cl, res, &error);

	if (o == NULL || !qmi_message_voice_stop_continuous_dtmf_output_get_result(o, &error)) {
		if (o)
			qmi_message_voice_stop_continuous_dtmf_output_unref(o);
		tones_done(x, error);
		return;
	}
	qmi_message_voice_stop_continuous_dtmf_output_unref(o);
	x->pos++;
	g_timeout_add(DTMF_OFF_MS, tone_gap_over, x);
}

static gboolean
tone_on_over(gpointer data)
{
	struct tones_ctx *x = data;
	QmiMessageVoiceStopContinuousDtmfInput *in = qmi_message_voice_stop_continuous_dtmf_input_new();

	qmi_message_voice_stop_continuous_dtmf_input_set_data(in, x->id, NULL);
	qmi_client_voice_stop_continuous_dtmf(VOICE, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)tone_stop_ready, x);
	qmi_message_voice_stop_continuous_dtmf_input_unref(in);
	return G_SOURCE_REMOVE;
}

static void
tone_start_ready(QmiClientVoice *cl, GAsyncResult *res, struct tones_ctx *x)
{
	GError *error = NULL;
	QmiMessageVoiceStartContinuousDtmfOutput *o = qmi_client_voice_start_continuous_dtmf_finish(cl, res, &error);

	if (o == NULL || !qmi_message_voice_start_continuous_dtmf_output_get_result(o, &error)) {
		if (o)
			qmi_message_voice_start_continuous_dtmf_output_unref(o);
		tones_done(x, error);
		return;
	}
	qmi_message_voice_start_continuous_dtmf_output_unref(o);
	g_timeout_add(DTMF_ON_MS, tone_on_over, x);
}

static void
tone_next(struct tones_ctx *x)
{
	QmiMessageVoiceStartContinuousDtmfInput *in;

	if (x->tones[x->pos] == 0) {
		mlog("voice: DTMF \"%s\" sent", x->tones);
		tones_done(x, NULL);
		return;
	}
	in = qmi_message_voice_start_continuous_dtmf_input_new();
	qmi_message_voice_start_continuous_dtmf_input_set_data(in, x->id,
	    (guint8)x->tones[x->pos], NULL);
	qmi_client_voice_start_continuous_dtmf(VOICE, in, TIMEOUT, NULL,
	    (GAsyncReadyCallback)tone_start_ready, x);
	qmi_message_voice_start_continuous_dtmf_input_unref(in);
}

void
qmi_send_tones(const char *tones, GDBusMethodInvocation *inv)
{
	struct call *active = NULL;
	struct tones_ctx *x;

	if (tones[0] == 0 || strspn(tones, "0123456789*#ABCDabcd") != strlen(tones)) {
		ofono_reply_error(inv, OFONO_ERROR_INVALID_FORMAT, tones);
		return;
	}
	for (GList *l = M.calls; l != NULL; l = l->next)
		if (((struct call *)l->data)->state == CALL_ACTIVE)
			active = l->data;
	if (active == NULL) {
		ofono_reply_error(inv, OFONO_ERROR_FAILED, "no active call");
		return;
	}
	x = g_new0(struct tones_ctx, 1);
	x->inv = inv;
	x->tones = g_ascii_strup(tones, -1);
	x->id = active->id;
	tone_next(x);
}

/* --- the clients ---------------------------------------------------------- */

static const struct svc {
	QmiService	svc;
	const char	*name;
	QmiClient	**slot;
	gboolean	*have;
	void		(*setup)(void);
} svcs[] = {
	{ QMI_SERVICE_DMS, "DMS", &cl_dms, &M.have_dms, dms_setup },
	{ QMI_SERVICE_UIM, "UIM", &cl_uim, &M.have_uim, uim_setup },
	{ QMI_SERVICE_NAS, "NAS", &cl_nas, &M.have_nas, nas_setup },
	{ QMI_SERVICE_WMS, "WMS", &cl_wms, &M.have_wms, NULL },
	{ QMI_SERVICE_VOICE, "VOICE", &cl_voice, &M.have_voice, voice_setup },
};
#define	NSVCS	(sizeof(svcs) / sizeof(svcs[0]))

static int alloc_tries[NSVCS];
static gboolean alloc_started[NSVCS];
static void alloc_next(guint i);

static gboolean
alloc_again(gpointer data)
{
	alloc_next(GPOINTER_TO_UINT(data));
	return G_SOURCE_REMOVE;
}

static void
alloc_ready(QmiDevice *d, GAsyncResult *res, gpointer data)
{
	guint i = GPOINTER_TO_UINT(data);
	const struct svc *s = &svcs[i];
	GError *error = NULL;
	QmiClient *c = qmi_device_allocate_client_finish(d, res, &error);

	if (c == NULL) {
		if (++alloc_tries[i] < ALLOC_TRIES) {
			if (alloc_tries[i] == 1)
				mlog("qmi: no %s client yet (%s): again every 5 s",
				    s->name, error->message);
			g_timeout_add_seconds(5, alloc_again, GUINT_TO_POINTER(i));
		} else {
			mlog("qmi: no %s client: %s; going on without it", s->name,
			    error->message);
		}
		g_error_free(error);
	} else {
		*s->slot = c;
		*s->have = TRUE;
		mlog("qmi: %s client %u", s->name, qmi_client_get_cid(c));
		if (s->setup)
			s->setup();
		/* WMS waits for the SIM, unless it is ready already */
		if (s->svc == QMI_SERVICE_WMS && M.sim_present &&
		    !g_strcmp0(M.pin_required, "none"))
			wms_setup();
		ofono_interfaces_changed();
	}
	/* the first round goes through all services in order */
	if (i + 1 < NSVCS && !alloc_started[i + 1])
		alloc_next(i + 1);
}

static void
alloc_next(guint i)
{
	alloc_started[i] = TRUE;
	qmi_device_allocate_client(dev, svcs[i].svc, QMI_CID_NONE, TIMEOUT, NULL,
	    (GAsyncReadyCallback)alloc_ready, GUINT_TO_POINTER(i));
}

static void
device_removed(QmiDevice *d, gpointer data)
{
	(void)d; (void)data;
	mlog("qmi: the modem went away (restarted or crashed)");
	modemd_quit(2);
}

void
qmi_start(QmiDevice *d)
{
	dev = d;
	g_signal_connect(dev, QMI_DEVICE_SIGNAL_REMOVED, G_CALLBACK(device_removed), NULL);
	alloc_next(0);
}

/* the clients released (their state on the modem's side goes with them) */
static void (*stop_done)(void);
static guint stop_pending;

static void
release_ready(QmiDevice *d, GAsyncResult *res, gpointer data)
{
	(void)data;
	qmi_device_release_client_finish(d, res, NULL);
	if (--stop_pending == 0 && stop_done)
		stop_done();
}

void
qmi_stop(void (*done)(void))
{
	stop_done = done;
	if (signal_timer)
		g_source_remove(signal_timer);
	signal_timer = 0;
	for (guint i = 0; i < NSVCS; i++) {
		if (*svcs[i].slot == NULL)
			continue;
		stop_pending++;
		qmi_device_release_client(dev, *svcs[i].slot,
		    QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, 3, NULL,
		    (GAsyncReadyCallback)release_ready, NULL);
	}
	if (stop_pending == 0 && done)
		done();
}
