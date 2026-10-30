/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-modemd: the Pro1X modem's calls, SMS and network registration on
 * FreeBSD (freebsd/modem/README.md). The modem side is QMI over QRTR
 * through libqmi-glib (qmi.c), the side the applications see a subset of
 * oFono's D-Bus API (ofono.c), so that Maemo Leste's sphone runs unchanged.
 *
 *   main.c    options, the QMI device (a QRTR node, or a QMUX file for the
 *             tests), the SIM PIN file, the call audio child
 *   qmi.c     DMS, UIM, NAS, WMS and VOICE clients, the modem's state
 *   ofono.c   org.ofono on the system bus: Manager, Modem, SimManager,
 *             NetworkRegistration, VoiceCallManager, VoiceCall,
 *             MessageManager, Message
 *   sms.c     SMS PDUs (KolibriOS for ARM's codec)
 *   data.c    mobile data: DPM, WDA, WDS sessions; ConnectionManager,
 *             ConnectionContext (freebsd/data/README.md)
 */

#ifndef _MODEMD_H_
#define	_MODEMD_H_

#include <gio/gio.h>
#include <libqmi-glib.h>
#include <time.h>

#define	MODEMD_NAME	"pro1x-modemd"

/* oFono's call states (voicecall-api.txt) */
enum call_state {
	CALL_ACTIVE,
	CALL_HELD,
	CALL_DIALING,
	CALL_ALERTING,
	CALL_INCOMING,
	CALL_WAITING,
	CALL_DISCONNECTED,
};

struct call {
	guint8		id;		/* the modem's call id (QMI VOICE) */
	guint		n;		/* the object's number */
	char		*path;		/* <modem>/voicecall01 */
	char		*number;	/* LineIdentification */
	enum call_state	state;
	gboolean	incoming;
	gboolean	emergency;
	char		start_time[32];	/* StartTime once active, ISO 8601 */
	guint		reg;		/* its D-Bus object */
	gboolean	seen;		/* in the modem's latest call list */
};

/* an outgoing SMS (oFono's Message object): its parts in flight */
struct message {
	guint		n;
	char		*path;		/* <modem>/message_<40 hex digits> */
	const char	*state;		/* "pending", "sent", "failed" */
	int		parts, sent, failed;
	guint		reg;
};

struct modem {
	char		*path;		/* the modem's object path */
	/* DMS */
	char		*manufacturer, *model, *revision, *serial;
	gboolean	powered, online;
	/* UIM */
	gboolean	sim_present;
	const char	*pin_required;	/* "none", "pin", "puk", "pin2", "puk2" */
	int		pin_retries, puk_retries;	/* -1: unknown */
	char		*imsi, *iccid;
	/* NAS */
	const char	*reg_status;	/* "unregistered", "registered", "searching", ... */
	const char	*technology;	/* "gsm", "umts", "lte", "nr", NULL */
	char		*op_name, *op_mcc, *op_mnc;
	guint16		lac;
	guint32		cell_id;
	gboolean	have_lac, have_cell;
	int		strength;	/* 0-100, -1: unknown */
	/* what the modem offers (its clients allocated) */
	gboolean	have_dms, have_uim, have_nas, have_wms, have_voice;
	/* the network's time (NAS indication), Unix; 0: none */
	time_t		net_time;
	int		net_tz;		/* minutes east of UTC */
	GList		*calls;		/* struct call */
	GList		*messages;	/* struct message, pending */
};

extern struct modem M;
extern int verbose;

void mlog(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void mdbg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* main.c */
struct options {
	const char	*device;	/* qrtr://N or a QMUX file */
	const char	*pin_file;	/* the SIM PIN (never logged, never written) */
	const char	*pin_tried;	/* marks a PIN attempt in this boot */
	int		pin_min_retries;	/* no attempt below this many tries */
	const char	*callaudio;	/* run per call, NULL: none */
	gboolean	online;		/* the modem online at the start */
	gboolean	session_bus;	/* the session bus instead (tests) */
	int		signal_poll;	/* seconds between signal readings */
};
extern struct options opt;
void calls_changed(void);	/* starts or stops the call audio child */
void modemd_quit(int code);

/* qmi.c: the modem side */
void qmi_start(QmiDevice *dev);
void qmi_stop(void (*done)(void));
void qmi_dial(const char *number, const char *clir, GDBusMethodInvocation *inv);
void qmi_answer(struct call *c, GDBusMethodInvocation *inv);
void qmi_hangup(struct call *c, GDBusMethodInvocation *inv);
void qmi_hangup_all(GDBusMethodInvocation *inv);
void qmi_send_tones(const char *tones, GDBusMethodInvocation *inv);
void qmi_send_sms(const char *to, const char *text, GDBusMethodInvocation *inv);
void qmi_set_online(gboolean on, GDBusMethodInvocation *inv);
void qmi_enter_pin(const char *type, const char *pin, GDBusMethodInvocation *inv);
void qmi_reset_pin(const char *type, const char *puk, const char *pin,
    GDBusMethodInvocation *inv);
void qmi_register(GDBusMethodInvocation *inv);
struct call *call_find_path(const char *path);
const char *call_state_name(enum call_state s);
gboolean calls_live(void);

/* ofono.c: the D-Bus side */
int ofono_init(GDBusConnection *bus);
void ofono_modem_changed(const char *prop);
void ofono_sim_changed(const char *prop);
void ofono_netreg_changed(const char *prop);
void ofono_interfaces_changed(void);
void ofono_call_added(struct call *c);
void ofono_call_changed(struct call *c, const char *prop);
void ofono_call_removed(struct call *c);
void ofono_incoming_message(const char *text, const char *sender, time_t t,
    int tz_minutes, gboolean immediate);
struct message *ofono_message_new(int parts);
void ofono_message_state(struct message *m, const char *state);
void ofono_reply_error(GDBusMethodInvocation *inv, const char *err, const char *msg);

/* data.c: mobile data over the IPA (freebsd/data/README.md) */
struct data_options {
	const char	*conf;		/* the user's APN file (-c) */
	const char	*hook;		/* puts the addresses on (-I) */
	const char	*wda;		/* only this QMAP format (-W) */
	const char	*ifprefix;	/* qmap: qmap0 is mux id 1 */
	const char	*ipa_sysctl;	/* qcom_ipa's sysctl tree */
	guint32		rx_ep, tx_ep;	/* the IPA's endpoint ids (-E rx,tx) */
	gboolean	disabled;	/* -D: no mobile data */
	gboolean	no_ifcheck;	/* -X: no interface needed (tests) */
};
extern struct data_options dopt;
int	data_init(GDBusConnection *bus);
void	data_start(QmiDevice *dev);
void	data_stop(void);
gboolean data_available(void);
void	data_netreg_changed(void);

#define	OFONO_ERROR_FAILED		"org.ofono.Error.Failed"
#define	OFONO_ERROR_IN_PROGRESS		"org.ofono.Error.InProgress"
#define	OFONO_ERROR_INVALID_ARGS	"org.ofono.Error.InvalidArguments"
#define	OFONO_ERROR_INVALID_FORMAT	"org.ofono.Error.InvalidFormat"
#define	OFONO_ERROR_NOT_IMPLEMENTED	"org.ofono.Error.NotImplemented"
#define	OFONO_ERROR_NOT_AVAILABLE	"org.ofono.Error.NotAvailable"
#define	OFONO_ERROR_INCORRECT_PASSWORD	"org.ofono.Error.IncorrectPassword"

#endif /* _MODEMD_H_ */
