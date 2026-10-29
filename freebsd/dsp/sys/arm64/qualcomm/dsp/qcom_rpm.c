/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Votes at the RPM (rpm.h, rpm_core.c): the PMIC's regulators that Linux's
 * qcom_smd-regulator drives through the RPM, for drivers on top (the
 * Wi-Fi's four rails, freebsd/dsp/README.md). FreeBSD has no RPM driver;
 * this sends Linux's requests on the channel "rpm_requests" that Linux
 * opened on the RPM's GLINK edge and that stays open across the kexec.
 *
 * From the device tree: the edge (compatible "qcom,glink-rpm": its message
 * RAM "qcom,rpm-msg-ram", the doorbell "mboxes" to the RPM), the
 * regulators below it ("qcom,rpm-pm6125-regulators": node l<n> is the
 * resource ldoa <n>, s<n> is smpa <n>, as in Linux's rpm_pm6125_regulators).
 * A regulator goes on as Linux's rpm_reg_enable does it the first time:
 * "swen" 1 and "uv" its regulator-min-microvolt (the regulator core sets a
 * voltage range's minimum before the first enable); off is "swen" 0.
 * Answers are polled for: requests are rare and come from threads that
 * may sleep.
 *
 *   sysctl hw.qcom_rpm              the edge, the counts, the votes
 *   sysctl hw.qcom_rpm.regulator="l8 1"   a regulator on (0: off)
 *   hw.qcom_rpm.lcid=N (loader)     the channel id, if Linux's is not found
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/sbuf.h>
#include <sys/sx.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>

#include "qcom_dsp.h"
#include "rpm.h"

#define	RPM_MAX_VOTES		16
#define	RPM_TIMEOUT_MS		5000	/* Linux's RPM_REQUEST_TIMEOUT */

static struct {
	struct sx	lock;
	int		state;		/* 0 not yet, 1 up, errno */
	struct rpm_edge	edge;
	void		*va;
	vm_paddr_t	pa;
	vm_size_t	size;
	phandle_t	ipc_xref;
	u_int		ipc_bit;
	phandle_t	regulators;	/* the PMIC's node below the edge */
	int		lcid;		/* tunable: 0 = Linux's, found */
	u_int		requests, refused, timeouts;
	struct {
		phandle_t	node;
		int		count;
	} votes[RPM_MAX_VOTES];
} rpm;

SX_SYSINIT(qcom_rpm_lock, &rpm.lock, "qcom_rpm");

/* --- the edge ------------------------------------------------------------------ */

static int
rpm_edge_up(void)
{
	phandle_t node, xref;
	bus_addr_t pa;
	bus_size_t size;
	int error;

	sx_assert(&rpm.lock, SA_XLOCKED);
	if (rpm.state != 0)
		return (rpm.state == 1 ? 0 : rpm.state);
	rpm.state = ENXIO;
	node = ofw_bus_find_compatible(OF_finddevice("/"), "qcom,glink-rpm");
	if (node <= 0 || OF_getencprop(node, "qcom,rpm-msg-ram", &xref,
	    sizeof(xref)) != sizeof(xref)) {
		printf("qcom_rpm: no RPM edge in the device tree\n");
		return (rpm.state);
	}
	if (ofw_reg_to_paddr(OF_node_from_xref(xref), 0, &pa, &size, NULL) != 0 ||
	    size < RPM_TOC_SIZE || size > 1024 * 1024) {
		printf("qcom_rpm: no message RAM\n");
		return (rpm.state);
	}
	if ((error = qcom_ipc_get(node, 0, &rpm.ipc_xref, &rpm.ipc_bit)) != 0) {
		printf("qcom_rpm: no doorbell to the RPM (%d)\n", error);
		return (rpm.state);
	}
	rpm.regulators = ofw_bus_find_compatible(node,
	    "qcom,rpm-pm6125-regulators");
	rpm.pa = pa;
	rpm.size = size;
	rpm.va = pmap_mapdev(pa, size);
	error = rpm_edge_init(&rpm.edge, rpm.va, size, (uint16_t)rpm.lcid);
	if (error != 0) {
		printf("qcom_rpm: message RAM %#jx: no table of contents or "
		    "FIFOs (%d)\n", (uintmax_t)pa, error);
		pmap_unmapdev(rpm.va, size);
		rpm.va = NULL;
		return (rpm.state);
	}
	printf("qcom_rpm: message RAM %#jx (%ju KiB), doorbell bit %u, FIFOs "
	    "of %u and %u bytes; channel %u%s (%u of Linux's requests seen), "
	    "message ids from %u\n", (uintmax_t)pa, (uintmax_t)size / 1024,
	    rpm.ipc_bit, rpm.edge.tx.len, rpm.edge.rx.len, rpm.edge.lcid,
	    rpm.lcid != 0 ? " (given)" : "", rpm.edge.lcid_seen,
	    rpm.edge.msg_id + 1);
	rpm.state = 1;
	return (0);
}

static void
rpm_sleep(int ms)
{
	pause_sbt("qcomrpm", ms * SBT_1MS, SBT_1MS, 0);
}

/* A request for the active or sleep set: 0, ENXIO/EINVAL refused, ETIMEDOUT */
int
qcom_rpm_write(int state, uint32_t type, uint32_t id, const uint32_t *kv,
    int npairs)
{
	uint32_t msg_id;
	int error, ms, r;

	sx_xlock(&rpm.lock);
	if ((error = rpm_edge_up()) != 0)
		goto out;
	/* room in the FIFO: the RPM reads it promptly */
	for (ms = 0; (error = rpm_send(&rpm.edge, state, type, id, kv, npairs,
	    &msg_id)) == ENOSPC && ms < RPM_TIMEOUT_MS; ms += 10)
		rpm_sleep(10);
	if (error != 0) {
		if (error == ENOSPC) {
			rpm.timeouts++;
			error = ETIMEDOUT;
		}
		goto out;
	}
	rpm.requests++;
	qcom_ipc_kick(rpm.ipc_xref, rpm.ipc_bit);
	for (ms = 0; (r = rpm_poll(&rpm.edge, msg_id)) == 0 &&
	    ms < RPM_TIMEOUT_MS; ms += ms < 20 ? 1 : 10)
		rpm_sleep(ms < 20 ? 1 : 10);
	if (r == 1)
		error = 0;
	else if (r == 0) {
		rpm.timeouts++;
		error = ETIMEDOUT;
		printf("qcom_rpm: no answer to request %u (%#x %u)\n", msg_id,
		    type, id);
	} else {
		rpm.refused++;
		error = -r;
		printf("qcom_rpm: request %u (%#x %u) refused: %s\n", msg_id,
		    type, id, rpm.edge.err);
	}
out:
	sx_xunlock(&rpm.lock);
	return (error);
}

/* --- regulators ------------------------------------------------------------------- */

/* Linux's rpm_pm6125_regulators: l1-l24 are ldoa, s1-s8 smpa */
static int
rpm_resource(phandle_t node, uint32_t *type, uint32_t *id, char *name,
    size_t len)
{
	char *end;
	u_long n;

	if (OF_getprop(node, "name", name, len) <= 0)
		return (ENOENT);
	name[len - 1] = '\0';
	if (!ofw_bus_node_is_compatible(OF_parent(node),
	    "qcom,rpm-pm6125-regulators"))
		return (EOPNOTSUPP);
	n = strtoul(name + 1, &end, 10);
	if (*end != '\0' && *end != '@')
		return (EINVAL);
	if (name[0] == 'l' && n >= 1 && n <= 24)
		*type = RPM_RES_LDOA;
	else if (name[0] == 's' && n >= 1 && n <= 8)
		*type = RPM_RES_SMPA;
	else
		return (EINVAL);
	*id = (uint32_t)n;
	return (0);
}

static int
rpm_vote(phandle_t node, bool on, const char *who)
{
	uint32_t kv[4], type, id;
	pcell_t uv = 0;
	char name[16];
	int error, i, slot = -1, n;

	if ((error = rpm_resource(node, &type, &id, name, sizeof(name))) != 0) {
		printf("qcom_rpm: %s: regulator %s: not an RPM regulator of the "
		    "PM6125 (%d)\n", who, name, error);
		return (error);
	}
	sx_xlock(&rpm.lock);
	for (i = 0; i < RPM_MAX_VOTES; i++) {
		if (rpm.votes[i].node == node)
			break;
		if (rpm.votes[i].node == 0 && slot < 0)
			slot = i;
	}
	if (i == RPM_MAX_VOTES) {
		if (!on || slot < 0) {
			sx_xunlock(&rpm.lock);
			return (on ? ENOSPC : 0);
		}
		i = slot;
		rpm.votes[i].node = node;
		rpm.votes[i].count = 0;
	}
	if ((on && rpm.votes[i].count++ > 0) ||
	    (!on && (rpm.votes[i].count == 0 || --rpm.votes[i].count > 0))) {
		sx_xunlock(&rpm.lock);
		return (0);
	}
	sx_xunlock(&rpm.lock);

	n = 0;
	kv[n++] = RPM_KEY_SWEN;
	kv[n++] = on ? 1 : 0;
	if (on && OF_getencprop(node, "regulator-min-microvolt", &uv,
	    sizeof(uv)) == sizeof(uv)) {
		kv[n++] = RPM_KEY_UV;
		kv[n++] = uv;
	}
	error = qcom_rpm_write(RPM_ACTIVE_STATE, type, id, kv, n / 2);
	if (on && n > 2)
		printf("qcom_rpm: %s: %s (%s %u) on at %u uV: %s (%d)\n", who,
		    name, type == RPM_RES_LDOA ? "ldoa" : "smpa", id, uv,
		    error == 0 ? "done" : "failed", error);
	else if (on)
		printf("qcom_rpm: %s: %s (%s %u) on at the RPM's voltage: %s "
		    "(%d)\n", who, name, type == RPM_RES_LDOA ? "ldoa" : "smpa",
		    id, error == 0 ? "done" : "failed", error);
	else
		printf("qcom_rpm: %s: %s (%s %u) off: %s (%d)\n", who, name,
		    type == RPM_RES_LDOA ? "ldoa" : "smpa", id,
		    error == 0 ? "done" : "failed", error);
	if (error != 0) {
		/* the vote did not happen */
		sx_xlock(&rpm.lock);
		rpm.votes[i].count += on ? -1 : 1;
		sx_xunlock(&rpm.lock);
	}
	return (error);
}

/* A regulator by the xref of its node (a consumer's <supply>-supply) */
int
qcom_rpm_regulator(phandle_t xref, bool on, const char *who)
{
	return (rpm_vote(OF_node_from_xref(xref), on, who));
}

/* --- sysctl -------------------------------------------------------------------------- */

static SYSCTL_NODE(_hw, OID_AUTO, qcom_rpm, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "RPM votes (freebsd/dsp)");
SYSCTL_INT(_hw_qcom_rpm, OID_AUTO, lcid, CTLFLAG_RDTUN, &rpm.lcid, 0,
    "The channel id to send on (0: Linux's, from its requests)");
SYSCTL_UINT(_hw_qcom_rpm, OID_AUTO, requests, CTLFLAG_RD, &rpm.requests, 0,
    "Requests acknowledged or refused");
SYSCTL_UINT(_hw_qcom_rpm, OID_AUTO, refused, CTLFLAG_RD, &rpm.refused, 0,
    "Requests refused");
SYSCTL_UINT(_hw_qcom_rpm, OID_AUTO, timeouts, CTLFLAG_RD, &rpm.timeouts, 0,
    "Requests not answered");

static int
rpm_edge_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	int error;

	sbuf_new_for_sysctl(&sb, NULL, 128, req);
	sx_xlock(&rpm.lock);
	error = rpm_edge_up();
	if (error == 0)
		sbuf_printf(&sb, "message RAM %#jx, channel %u (%u of Linux's "
		    "requests), last message id %u", (uintmax_t)rpm.pa,
		    rpm.edge.lcid, rpm.edge.lcid_seen, rpm.edge.msg_id);
	else
		sbuf_printf(&sb, "not up (%d)", error);
	sx_xunlock(&rpm.lock);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}
SYSCTL_PROC(_hw_qcom_rpm, OID_AUTO, edge,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, rpm_edge_sysctl,
    "A", "The RPM's GLINK edge (brought up on reading)");

/* read: the votes held ("l8=1 ..."); write "<name> <0|1>": vote */
static int
rpm_regulator_sysctl(SYSCTL_HANDLER_ARGS)
{
	char buf[32], name[16], *sp;
	struct sbuf sb;
	phandle_t regs, node;
	int error, i, on;

	if (req->newptr == NULL) {
		sbuf_new_for_sysctl(&sb, NULL, 128, req);
		sx_xlock(&rpm.lock);
		for (i = 0; i < RPM_MAX_VOTES; i++)
			if (rpm.votes[i].node != 0 && OF_getprop(rpm.votes[i].node,
			    "name", name, sizeof(name)) > 0) {
				name[sizeof(name) - 1] = '\0';
				sbuf_printf(&sb, "%s%s=%d", sbuf_len(&sb) > 0 ?
				    " " : "", name, rpm.votes[i].count);
			}
		sx_xunlock(&rpm.lock);
		error = sbuf_finish(&sb);
		sbuf_delete(&sb);
		return (error);
	}
	if (req->newlen >= sizeof(buf))
		return (EINVAL);
	error = SYSCTL_IN(req, buf, req->newlen);
	if (error != 0)
		return (error);
	buf[req->newlen] = '\0';
	sp = strchr(buf, ' ');
	if (sp == NULL)
		return (EINVAL);
	*sp++ = '\0';
	on = (int)strtol(sp, NULL, 10);
	sx_xlock(&rpm.lock);
	error = rpm_edge_up();
	regs = rpm.regulators;
	sx_xunlock(&rpm.lock);
	if (error != 0)
		return (error);
	if (regs <= 0 || (node = ofw_bus_find_child(regs, buf)) <= 0)
		return (ENOENT);
	return (rpm_vote(node, on != 0, "sysctl"));
}
SYSCTL_PROC(_hw_qcom_rpm, OID_AUTO, regulator,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    rpm_regulator_sysctl, "A",
    "Votes held; write \"<regulator> <0|1>\" (e.g. \"l8 1\") to vote");
