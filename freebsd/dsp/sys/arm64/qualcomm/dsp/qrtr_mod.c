/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The module qrtr: the router with its name service (qrtr_core.c), the
 * sockets (qrtr_sock.c) and the endpoints over GLINK (qrtr_glink.c).
 * sysctl net.qrtr.servers lists what the name service knows.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>

#include "qrtr.h"

static void
server_line(void *arg, uint32_t service, uint32_t instance, uint32_t node,
    uint32_t port)
{
	sbuf_printf(arg, "%u %u %u %u\n", service, instance, node, port);
}

static int
sysctl_servers(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	int error;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	sbuf_printf(&sb, "\nservice instance node port\n");
	qrtr_ns_servers(server_line, &sb);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

SYSCTL_NODE(_net, OID_AUTO, qrtr, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Qualcomm IPC router");
SYSCTL_PROC(_net_qrtr, OID_AUTO, servers, CTLTYPE_STRING | CTLFLAG_RD |
    CTLFLAG_MPSAFE, NULL, 0, sysctl_servers, "A",
    "Servers known to the name service: service instance node port");

static int
qrtr_modevent(module_t mod, int what, void *arg)
{
	int error;

	switch (what) {
	case MOD_LOAD:
		if ((error = qrtr_init()) != 0)
			return (error);
		qrtr_glink_init();
		printf("qrtr: node %u, name service on port %#x\n",
		    QRTR_LOCAL_NODE, QRTR_PORT_CTRL);
		return (0);
	case MOD_UNLOAD:
		/* the socket family cannot go while sockets may exist */
		return (EBUSY);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t qrtr_mod = { "qrtr", qrtr_modevent, NULL };
/* after the socket family (SI_SUB_PROTO_DOMAIN), before any socket */
DECLARE_MODULE(qrtr, qrtr_mod, SI_SUB_PROTO_DOMAIN, SI_ORDER_SECOND);
MODULE_VERSION(qrtr, 1);
MODULE_DEPEND(qrtr, qcom_dsp, 1, 1, 1);
