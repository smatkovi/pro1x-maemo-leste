/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Between the simulation's parts (freebsd/dsp/test/sim): the kernel side
 * (simkern.c, fdt.c), the hardware (hw.c: SMEM as the boot loader lays it
 * out, the TCSR mutex, APCS doorbells, the GIC, TrustZone), the remote
 * processors (remote.c) and the scenarios (sim.c).
 */

#ifndef _SIM_H_
#define	_SIM_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct sim_device;

/* the console: everything the drivers print, kept for the checks */
int	sim_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
size_t	sim_log_mark(void);
bool	sim_log_has(size_t since, const char *needle);
int	sim_log_count(size_t since, const char *needle);
void	sim_log_quiet(bool quiet);

/* device tree, newbus, modules */
int	sim_fdt_load(const char *path);
int	sim_fdt_nnodes(void);
uint32_t sim_device_node(struct sim_device *dev);
void	sim_bus_attach(void);		/* the drivers registered so far */
int	sim_module_load(const char *name);

/* interrupts: a thread per line, as FreeBSD's ithreads */
void	sim_irq_raise(int irq);
#define	SIM_SPI(n)	(32 + (n))

/* physical memory: the simulated regions */
void	*sim_region_add(uint64_t pa, uint64_t size, const char *name);
void	*sim_phys(uint64_t pa, uint64_t len);

/* hw.c */
void	sim_hw_init(void);
/* SMEM as XBL leaves it (version 12, partitions with hosts 1, 2, 5) */
void	sim_smem_boot(void);
/* the remote's side: items in the partition with host, uncached */
void	*sim_smem_alloc(int host, unsigned item, size_t size);
void	*sim_smem_get(int host, unsigned item, size_t *size);
/* TrustZone's view */
struct sim_tz {
	int	calls, interrupted, busy;
	int	init_image, mem_setup, auth_reset, shutdown, assign;
	int	auth_fail;		/* the last auth's segment check */
	uint32_t rmtfs_owners[4];	/* VMIDs owning the rmtfs region */
	int	nowners;
	int	interrupt_next;		/* the next PIL call returns 1 once */
	int	busy_next;		/* ... -12 once */
};
extern struct sim_tz sim_tz;
/* doorbells: APCS IPC bit -> who listens (remote.c) */
typedef void sim_doorbell_cb_t(void *arg, int bit);
void	sim_doorbell_listen(int bit, sim_doorbell_cb_t *cb, void *arg);

/* remote.c: the remote processors */
struct remote;
enum remote_quirk {
	RQ_NONE = 0,
	RQ_READY_BEFORE_IRQ = 1,	/* item appears with ready already set */
	RQ_NO_SMP2P_IRQ = 2,		/* ... and no interrupt at all */
	RQ_HELLO_AT_ONCE = 4,		/* HELLO with the first intent of ours */
	RQ_NEEDS_RMTFS = 8,		/* fatal unless rmtfs is announced */
	RQ_CRASH_AFTER = 16,		/* fatal error after a while */
};
struct remote *sim_remote(const char *name);	/* "adsp", "modem", "cdsp" */
void	sim_remote_set_quirks(struct remote *r, int quirks);
/* TrustZone released it from reset / shut it down */
void	sim_remote_boot(int pas_id);
void	sim_remote_shutdown(int pas_id);
/* the kexec case: SMEM, SMP2P, GLINK as Linux left them, the remote running */
void	sim_remote_linux_left_running(struct remote *r);
/* what the remote saw */
struct remote_stats {
	bool	booted, ready, crashed, stopped;
	bool	glink_version;		/* our VERSION came */
	bool	ipcrtr_open;		/* both sides */
	int	hello_rx;		/* QRTR HELLOs from node 1 */
	int	servers_rx;		/* NEW_SERVER from node 1 */
	int	qmi_req, qmi_resp;
	int	rmtfs_opened;		/* its rmtfs client got an answer */
	int	bounced_tx;		/* sent into Linux's old intents */
	int	errors;
	uint32_t servers_seen[16];	/* services node 1 announced */
	int	nservers_seen;
};
void	sim_remote_stats(struct remote *r, struct remote_stats *st);
uint32_t sim_remote_qrtr_node(struct remote *r);

#endif /* _SIM_H_ */
