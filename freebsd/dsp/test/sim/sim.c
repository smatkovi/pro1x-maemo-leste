/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The DSP stack's start on the PC (freebsd/dsp/README.md, "The
 * simulation"): FreeBSD's drivers (qcom_dsp.ko's and qrtr.ko's files) and
 * the loader pro1x-dsp against the device tree, SMEM as the boot loader
 * leaves it, a TrustZone that checks the images, and remote processors
 * that answer as the real ones do (remote.c), run by run as on the phone:
 *
 *   sim [-q] [-d DTB] dsp      dsp1 (load), dsp3/dsp4 (ADSP: start, GLINK,
 *                              QRTR, a late user, stop, restart, crash),
 *                              dsp6 (modem: wrong start order, then
 *                              rmtfs, tqftpserv, pd-mapper first, DMS)
 *   sim ... ready-early        the ADSP's item appears with ready set
 *   sim ... no-irq             ... and it never interrupts
 *   sim ... adopt              the modem as Linux left it (kexec from
 *                              Leste): taken over, IPCRTR adopted
 *
 * The userspace daemons (rmtfs, tqftpserv, pd-mapper, qmicli) are ports
 * on the router here, as their sockets are on the phone.
 */

#include "simkern.h"
#include "sim.h"
#include "simfw.h"
#include "qrtr.h"
#include "glink.h"
#include "qcom_dsp.h"

#include <unistd.h>

#undef printf
#undef malloc
#undef free
#define	say(...)	sim_printf(__VA_ARGS__)

int	sim_loader(int argc, char **argv);

static int passes, fails;
static char fwdir[256];

static void
check(bool ok, const char *what)
{
	say("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (ok)
		passes++;
	else
		fails++;
}

/* wait up to ms for a condition */
#define	WAIT(cond, ms) ({						\
	int _w;								\
	for (_w = 0; _w < (ms) / 10 && !(cond); _w++)			\
		usleep(10000);						\
	(cond);								\
})

static bool
sysctl_has(const char *name, const char *needle)
{
	static char buf[16384];

	return (sim_sysctl(name, buf, sizeof(buf)) == 0 &&
	    strstr(buf, needle) != NULL);
}

static void
show(const char *name)
{
	static char buf[16384];

	if (sim_sysctl(name, buf, sizeof(buf)) == 0)
		say("%s: %s\n", name, buf);
}

static int
loader(const char *cmd, const char *name, const char *fw)
{
	char *argv[5] = { "pro1x-dsp", (char *)cmd, (char *)name, (char *)fw,
	    NULL };

	return (sim_loader(fw != NULL ? 4 : 3, argv));
}

/* --- the RPM (qcom_rpm.c's votes): what was asked ------------------------------- */

static struct {
	int		state;
	uint32_t	type, id, key, value;
} rpmlog[32];
static int nrpm;

int
qcom_rpm_write(int state, uint32_t type, uint32_t id, const uint32_t *kv,
    int npairs)
{
	if (nrpm < 32) {
		rpmlog[nrpm].state = state;
		rpmlog[nrpm].type = type;
		rpmlog[nrpm].id = id;
		rpmlog[nrpm].key = kv[0];
		rpmlog[nrpm].value = kv[1];
		nrpm++;
	}
	return (0);
}

int
qcom_rpm_regulator(phandle_t xref, bool on, const char *who)
{
	return (0);
}

void
qcom_fastrpc_init(void)
{
}

/* --- the daemons: ports on the router ------------------------------------------- */

struct daemon {
	const char	*name;
	uint32_t	service, instance;
	struct qrtr_port *port;
	int		requests;
	char		reply[128];
	bool		replied;
};

static void
qmi_resp_hdr(uint8_t *m, const uint8_t *req, uint16_t len)
{
	m[0] = 2;
	m[1] = req[1];
	m[2] = req[2];
	m[3] = req[3];
	m[4] = req[4];
	memcpy(m + 5, &len, 2);
}

static void
daemon_rx(void *arg, const struct qrtr_rx_info *info, const void *data,
    size_t len)
{
	struct daemon *d = arg;
	const uint8_t *q = data;
	uint8_t m[32];
	uint32_t caller = 1;
	uint16_t res[2] = { 0, 0 }, l;

	/* the name service's notices (BYE, DEL_CLIENT): not QMI, as libqrtr
	 * and qmi_core tell them apart, by the port they come from */
	if (info->src_port == QRTR_PORT_CTRL || len < 7)
		return;
	if (q[0] == 2) {
		/* a response (qmicli) */
		snprintf(d->reply, sizeof(d->reply), "%.*s",
		    (int)MIN(len, sizeof(d->reply) - 1), (const char *)q);
		if (len > 7 + 7 + 3)
			snprintf(d->reply, sizeof(d->reply), "%.*s",
			    (int)(len - 7 - 7 - 3), (const char *)q + 7 + 7 + 3);
		d->replied = true;
		return;
	}
	d->requests++;
	if (d->service != 14)
		return;
	/* rmtfs: OPEN answered with a caller id (Linux's rmtfs, qmi_rmtfs) */
	m[7] = 0x02;
	l = 4;
	memcpy(m + 8, &l, 2);
	memcpy(m + 10, res, 4);
	m[14] = 0x10;
	memcpy(m + 15, &l, 2);
	memcpy(m + 17, &caller, 4);
	qmi_resp_hdr(m, q, 14);
	qrtr_sendto(d->port, info->src_node, info->src_port, m, 21);
}

/* As libqrtr's qrtr_publish: NEW_SERVER to the local name service */
static void
daemon_start(struct daemon *d)
{
	struct qrtr_ctrl_pkt pkt;
	int error;

	error = qrtr_port_open(0, daemon_rx, d, &d->port);
	if (error != 0) {
		say("sim: %s: port: %d\n", d->name, error);
		return;
	}
	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_SERVER;
	pkt.server.service = d->service;
	pkt.server.instance = d->instance;
	qrtr_sendto(d->port, QRTR_LOCAL_NODE, QRTR_PORT_CTRL, &pkt, sizeof(pkt));
	say("sim: %s serving %u on port %#x\n", d->name, d->service,
	    qrtr_port_id(d->port));
}

static struct daemon pdmapper = { "pd-mapper", 64, 0x101 };
static struct daemon tqftp = { "tqftpserv", 4096, 1 };
static struct daemon rmtfs = { "rmtfs", 14, 1 };
static struct daemon qmicli = { "qmicli", 0, 0 };

/* Node and port of service svc from net.qrtr.servers (as the test does) */
static bool
server_of(uint32_t svc, uint32_t *node, uint32_t *port)
{
	static char buf[16384];
	char *l;
	unsigned s, i, n, p;

	if (sim_sysctl("net.qrtr.servers", buf, sizeof(buf)) != 0)
		return (false);
	for (l = strtok(buf, "\n"); l != NULL; l = strtok(NULL, "\n"))
		if (sscanf(l, "%u %u %u %u", &s, &i, &n, &p) == 4 && s == svc) {
			if (node != NULL)
				*node = n;
			if (port != NULL)
				*port = p;
			return (true);
		}
	return (false);
}

/* qmicli --dms-get-revision: QMI DMS 0x23 to the modem's DMS */
static bool
dms_revision(void)
{
	uint8_t req[7] = { 0, 5, 0, 0x23, 0, 0, 0 };
	uint32_t node, port;

	if (!server_of(2, &node, &port))
		return (false);
	if (qmicli.port == NULL)
		daemon_start(&qmicli);
	qmicli.replied = false;
	qrtr_sendto(qmicli.port, node, port, req, sizeof(req));
	if (!WAIT(qmicli.replied, 3000))
		return (false);
	say("sim: qmicli: revision '%s'\n", qmicli.reply);
	return (strstr(qmicli.reply, "MPSS.SIM") != NULL);
}

/* A late user of the ADSP's apr_audio_svc (as qcom_apr.ko loaded later) */
static struct glink_channel *apr_ch;
static volatile int apr_announced;

static void
apr_rx(void *arg, const void *data, size_t len)
{
}

static void
apr_cb(void *arg, struct glink_edge *edge, const char *label,
    const char *channel)
{
	if (glink_open(edge, channel, 2, 512, apr_rx, NULL, NULL, 5000,
	    &apr_ch) == 0)
		apr_announced = 1;
	else
		apr_announced = -1;
}

/* --- runs -------------------------------------------------------------------------------- */

static void
fw(const char *name, bool split, char *path, size_t len)
{
	snprintf(path, len, "%s/%s.%s", fwdir, name, split ? "mdt" : "mbn");
	if (simfw_write(path, name, split) != 0)
		say("sim: cannot write %s\n", path);
}

static void
setup(const char *dtb)
{
	int error;

	sim_hw_init();
	if ((error = sim_fdt_load(dtb)) != 0) {
		say("sim: device tree %s: %d\n", dtb, error);
		exit(2);
	}
	say("sim: device tree %s, %d nodes\n", dtb, sim_fdt_nnodes());
	snprintf(fwdir, sizeof(fwdir), "%s/dspsim.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	if (mkdtemp(fwdir) == NULL) {
		say("sim: no temporary directory\n");
		exit(2);
	}
}

/* dsp1: kldload qcom_dsp, qrtr */
static void
run_load(bool adopted)
{
	size_t m = sim_log_mark();

	check(sim_module_load("qcom_dsp") == 0, "dsp1: kldload qcom_dsp");
	check(sim_log_has(m, "qcom_dsp: secure monitor answers (0)"),
	    "dsp1: the secure monitor answers (SCM INFO call)");
	check(sim_log_has(m, "qcom_smem: 0x46000000, 2048 KB, 512 items"),
	    "dsp1: SMEM found (version 12, 512 items)");
	check(sim_log_has(m, "partition with host 1") &&
	    sim_log_has(m, "partition with host 2") &&
	    sim_log_has(m, "partition with host 5") &&
	    !sim_log_has(m, "partition with host 0:"),
	    "dsp1: SMEM partitions with the modem, ADSP, CDSP");
	check(sim_log_count(m, ": pid 0 <-> ") == 3,
	    "dsp1: three SMP2P edges attached");
	check(sim_cdev_find("qcom_pas/adsp") != NULL &&
	    sim_cdev_find("qcom_pas/modem") != NULL &&
	    sim_cdev_find("qcom_pas/cdsp") != NULL,
	    "dsp1: /dev/qcom_pas/{adsp,modem,cdsp}");
	check(sim_log_has(m, "adsp: PAS id 1 supported") &&
	    sim_log_has(m, "modem: PAS id 4 supported"),
	    "dsp1: PAS ids 1 and 4 supported by TrustZone");
	check(sysctl_has("hw.qcom_socinfo", "soc_id 417"),
	    "dsp1: hw.qcom_socinfo: SM6115 (soc_id 417)");
	check(sysctl_has("hw.qcom_rmtfs.assigned", "0") &&
	    sim_tz.nowners == 3 && sim_tz.rmtfs_owners[1] == 0xf &&
	    sim_tz.rmtfs_owners[2] == 0x2b,
	    adopted ? "dsp1: rmtfs region, shared before, shared again" :
	    "dsp1: rmtfs region shared with VMIDs 15, 43 (TrustZone's owners)");
	if (adopted)
		check(sim_log_has(m, "again, from VMIDs 3,15,43 (shared before?): 0"),
		    "dsp1: rmtfs's second call from its owners");
	check(sim_module_load("qrtr") == 0, "dsp1: kldload qrtr");
	show("hw.qcom_smp2p");
}

static void
run_adsp(int quirks, bool full)
{
	struct remote *r = sim_remote("adsp");
	struct remote_stats st;
	char path[512];
	size_t m;
	int tz_auth;

	/* Linux's order: pd-mapper before the DSP (pdm subdevice prepare) */
	if (pdmapper.port == NULL)
		daemon_start(&pdmapper);
	check(WAIT(server_of(64, NULL, NULL), 1000),
	    "dsp3: pd-mapper's service 64 listed before the start");
	fw("adsp", false, path, sizeof(path));
	memset(sim_phys(0x53800000ULL, 0x10000), 0xa5, 0x10000);
	sim_remote_set_quirks(r, quirks | RQ_HELLO_AT_ONCE);
	m = sim_log_mark();
	tz_auth = sim_tz.auth_reset;
	check(loader("start", "adsp", path) == 0, "dsp3: pro1x-dsp start adsp");
	check(sim_tz.auth_reset == tz_auth + 1 && sim_tz.auth_fail == 0,
	    "dsp3: TrustZone: metadata, memory setup, segments as hashed");
	check(sim_log_has(m, "adsp: running (ready after"),
	    "dsp3: ready over SMP2P");
	if (quirks & RQ_NO_SMP2P_IRQ)
		check(sim_log_has(m, "(read, no interrupt)"),
		    "dsp3: ready read without an interrupt");
	check(sim_log_has(m, "glink edge lpass up"), "dsp4: GLINK edge lpass up");
	check(WAIT(sim_log_has(m, "qrtr_glink: lpass up"), 3000),
	    "dsp4: IPCRTR open, QRTR endpoint");
	check(WAIT(server_of(43, NULL, NULL), 3000),
	    "dsp4: the ADSP's services in net.qrtr.servers (node 5)");
	sim_remote_stats(r, &st);
	check(st.glink_version && st.hello_rx >= 1 && st.errors == 0,
	    "dsp4: the ADSP got VERSION and our HELLO, no protocol error");
	check(WAIT((sim_remote_stats(r, &st), st.nservers_seen > 0), 2000),
	    "dsp4: our name service told the ADSP our servers (pd-mapper)");
	check(sysctl_has("hw.qcom_glink", "IPCRTR") &&
	    sysctl_has("hw.qcom_glink", "remote sent VERSION"),
	    "dsp4: hw.qcom_glink lists IPCRTR, the negotiation");
	show("hw.qcom_glink");
	show("net.qrtr.nodes");
	if (!full)
		return;
	/* a user that comes after the ADSP opened its channel */
	qcom_glink_register("apr_audio_svc", apr_cb, NULL);
	check(WAIT(apr_announced != 0, 6000) && apr_announced == 1,
	    "dsp4: a late user is told of apr_audio_svc, opens it");
	if (apr_ch != NULL) {
		glink_close(apr_ch);
		apr_ch = NULL;
	}
	qcom_glink_unregister("apr_audio_svc");

	/* stop, start again */
	m = sim_log_mark();
	check(loader("stop", "adsp", NULL) == 0, "dsp3: pro1x-dsp stop adsp");
	sim_remote_stats(r, &st);
	check(st.stopped && sim_log_has(m, "stop-ack after"),
	    "dsp3: the stop asked over SMP2P, acknowledged");
	check(WAIT(!server_of(43, NULL, NULL), 2000) &&
	    sim_log_has(m, "qrtr_glink: lpass gone"),
	    "dsp4: its services gone with it (BYE)");
	m = sim_log_mark();
	check(loader("start", "adsp", path) == 0 &&
	    sim_log_has(m, "adsp: running (ready after"),
	    "dsp3: started again (stale bits from the first start ignored)");
	check(WAIT(server_of(43, NULL, NULL), 3000),
	    "dsp4: its services back");
	check(loader("stop", "adsp", NULL) == 0, "dsp3: stopped again");

	/* a crash */
	sim_remote_set_quirks(r, RQ_CRASH_AFTER | RQ_HELLO_AT_ONCE);
	m = sim_log_mark();
	check(loader("start", "adsp", path) == 0, "dsp3: started (to crash)");
	check(WAIT(sim_log_has(m, "fatal error: err_fatal: simulated crash"),
	    3000), "dsp3: its fatal error and the reason from SMEM item 423");
	check(WAIT(!server_of(43, NULL, NULL), 2000),
	    "dsp4: the crashed ADSP's services gone");
	check(loader("stop", "adsp", NULL) == 0, "dsp3: a crashed ADSP shut down");
	sim_remote_set_quirks(r, RQ_HELLO_AT_ONCE);
}

static void
run_modem(void)
{
	struct remote *r = sim_remote("modem");
	struct remote_stats st;
	char path[512];
	size_t m;
	int k, swen = 0, lvl = 0;

	check(sim_sysctl_set_int("hw.qcom_pas.proxy_votes", 1) == 0,
	    "dsp6: hw.qcom_pas.proxy_votes=1");
	fw("modem", true, path, sizeof(path));
	sim_remote_set_quirks(r, RQ_NEEDS_RMTFS | RQ_HELLO_AT_ONCE);
	/* the wrong order: no rmtfs yet */
	m = sim_log_mark();
	nrpm = 0;
	sim_tz.interrupt_next = 1;
	sim_tz.busy_next = 1;
	check(loader("start", "modem", path) == 0,
	    "dsp6: pro1x-dsp start modem (split .mdt/.bNN image)");
	check(sim_tz.interrupted >= 1 && sim_tz.busy >= 1,
	    "dsp6: an interrupted and a busy SCM call resumed");
	check(WAIT(sim_log_has(m, "rmtfs not found"), 5000),
	    "dsp6: without rmtfs the modem fails (the order matters)");
	loader("stop", "modem", NULL);
	/* the order of freebsd/dsp/README.md: the servers, then the modem */
	daemon_start(&rmtfs);
	daemon_start(&tqftp);
	check(WAIT(server_of(14, NULL, NULL) && server_of(4096, NULL, NULL) &&
	    server_of(64, NULL, NULL), 1000),
	    "dsp6: rmtfs (14), tqftpserv (4096), pd-mapper (64) listed first");
	m = sim_log_mark();
	check(loader("start", "modem", path) == 0 &&
	    sim_log_has(m, "modem: running (ready after"),
	    "dsp6: the modem ready");
	for (k = 0; k < nrpm; k++) {
		if (rpmlog[k].type == 0x78637772 && rpmlog[k].key == 0x6e657773 &&
		    rpmlog[k].value == 1)
			swen++;
		if (rpmlog[k].type == 0x78637772 && rpmlog[k].key == 0x6c766c76 &&
		    rpmlog[k].value == 416)
			lvl++;
	}
	/* (sent at the first start, kept: FreeBSD cannot lower it again) */
	check(swen == 1 && lvl == 2 && nrpm == 3,
	    "dsp6: Linux's proxy vote: rwcx swen 1, vlvl 416 active and sleep");
	check(WAIT(sim_log_has(m, "qrtr_glink: mpss up"), 3000),
	    "dsp6: the modem's IPCRTR, QRTR endpoint");
	check(WAIT((sim_remote_stats(r, &st), st.rmtfs_opened > 0), 4000),
	    "dsp6: the modem found rmtfs and opened modem_fs1");
	check(WAIT(server_of(2, NULL, NULL) && server_of(69, NULL, NULL), 3000),
	    "dsp6: the modem's QMI services listed (DMS 2, WLFW 69)");
	check(dms_revision(), "dsp6: DMS answers get-revision");
	sim_remote_stats(r, &st);
	check(!st.crashed && st.errors == 0, "dsp6: the modem did not fail");
	show("net.qrtr.servers");
	check(loader("stop", "modem", NULL) == 0, "dsp6: pro1x-dsp stop modem");
}

static void
run_adopt(void)
{
	struct remote *r = sim_remote("modem");
	struct remote_stats st;
	size_t m;

	/* Linux's era: the modem running, rmtfs_mem's assignment */
	sim_tz.rmtfs_owners[0] = 3;
	sim_tz.rmtfs_owners[1] = 0xf;
	sim_tz.rmtfs_owners[2] = 0x2b;
	sim_tz.nowners = 3;
	sim_remote_set_quirks(r, 0);
	sim_remote_linux_left_running(r);
	m = sim_log_mark();
	run_load(true);
	check(sim_log_has(m, "remote running: our values kept") &&
	    sysctl_has("hw.qcom_smp2p", "ipa              0x00000003"),
	    "adopt: SMP2P: Linux's values kept (the modem's IPA bits)");
	daemon_start(&rmtfs);
	daemon_start(&tqftp);
	daemon_start(&pdmapper);
	m = sim_log_mark();
	check(loader("adopt", "modem", NULL) == 0, "adopt: pro1x-dsp adopt modem");
	check(sim_log_has(m, "IPCRTR: Linux's id 3 (QRTR data, to node 0), the "
	    "remote's 7 (QRTR data, from node 0)"),
	    "adopt: IPCRTR's ids found in the FIFOs' past");
	check(sim_tz.init_image == 0 && sim_tz.auth_reset == 0,
	    "adopt: nothing went to TrustZone's PAS");
	check(WAIT(sim_log_has(m, "qrtr_glink: mpss up (Linux's channel adopted)"),
	    3000), "adopt: QRTR on the adopted channel");
	check(WAIT(server_of(2, NULL, NULL), 3000),
	    "adopt: the modem told its servers again (HELLO from node 1)");
	check(WAIT((sim_remote_stats(r, &st), st.rmtfs_opened >= 2), 4000),
	    "adopt: the modem found our rmtfs and opened modem_fs1 again");
	sim_remote_stats(r, &st);
	check(st.bounced_tx > 0 && sysctl_has("hw.qcom_glink", "adopted"),
	    "adopt: data into Linux's intents taken (bounce), then ours");
	check(dms_revision(), "adopt: DMS answers get-revision");
	sim_remote_stats(r, &st);
	check(st.ipa_changes == 0 && sysctl_has("hw.qcom_smp2p",
	    "ipa              0x00000003"),
	    "adopt: our IPA bits never changed under the modem (SMP2P kept)");
	check(st.hello_to_me >= 1,
	    "adopt: our HELLO addressed to the modem's node (from the FIFOs)");
	check(st.errors == 0 && !st.crashed, "adopt: no protocol error");
	show("hw.qcom_glink");
	show("net.qrtr.nodes");
}

int
main(int argc, char **argv)
{
	const char *dtb = "pro1x-dsp.dtb", *what;
	int c;

	while ((c = getopt(argc, argv, "qd:")) != -1)
		switch (c) {
		case 'q':
			sim_log_quiet(true);
			break;
		case 'd':
			dtb = optarg;
			break;
		default:
			return (2);
		}
	what = optind < argc ? argv[optind] : "dsp";
	setbuf(stdout, NULL);
	setup(dtb);
	if (strcmp(what, "dsp") == 0) {
		run_load(false);
		run_adsp(0, true);
		run_modem();
	} else if (strcmp(what, "ready-early") == 0) {
		run_load(false);
		run_adsp(RQ_READY_BEFORE_IRQ, false);
	} else if (strcmp(what, "no-irq") == 0) {
		run_load(false);
		run_adsp(RQ_NO_SMP2P_IRQ, false);
	} else if (strcmp(what, "adopt") == 0)
		run_adopt();
	else {
		say("sim: what is %s?\n", what);
		return (2);
	}
	/* what the drivers' threads still do */
	usleep(500000);
	/* the firmware images (the processors are stopped or adopted) */
	{
		char cmd[300];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fwdir);
		if (system(cmd) != 0)
			say("sim: %s left\n", fwdir);
	}
	sim_log_quiet(false);
	say("=== sim %s: %s (%d passed, %d failed)\n", what,
	    fails == 0 ? "PASS" : "FAIL", passes, fails);
	/* the drivers' threads run on: no teardown, as on the phone */
	fflush(stdout);
	_exit(fails == 0 ? 0 : 1);
}
