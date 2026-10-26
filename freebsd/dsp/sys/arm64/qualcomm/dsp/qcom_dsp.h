/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The Qualcomm DSP substrate for FreeBSD (module qcom_dsp): secure monitor
 * calls, shared memory (SMEM) with its hardware mutex, the APCS IPC
 * doorbells, SMP2P bit states, and the remote processors' PAS interface.
 * Modelled on Linux's qcom_scm, qcom_smem, qcom_hwspinlock, smp2p and
 * qcom_q6v5_pas; freebsd/dsp/README.md.
 */

#ifndef _QCOM_DSP_H_
#define	_QCOM_DSP_H_

#include <sys/types.h>
#include <sys/bus.h>
#include <dev/ofw/openfirm.h>

/* Secure monitor calls (qcom_scm.c) */
int	qcom_scm_available(void);
int	qcom_scm_pas_supported(uint32_t pas_id, bool *supported);
int	qcom_scm_pas_init_image(uint32_t pas_id, vm_paddr_t metadata);
int	qcom_scm_pas_mem_setup(uint32_t pas_id, vm_paddr_t addr, vm_size_t size);
int	qcom_scm_pas_auth_and_reset(uint32_t pas_id);
int	qcom_scm_pas_shutdown(uint32_t pas_id);

/* Shared memory (qcom_smem.c); host: the remote's SMEM host id, or ANY */
#define	QCOM_SMEM_HOST_ANY	(-1)
int	qcom_smem_init(void);
int	qcom_smem_alloc(int host, u_int item, size_t size);
void	*qcom_smem_get(int host, u_int item, size_t *size, int *error);
vm_paddr_t qcom_smem_virt_to_phys(void *p);
void	qcom_smem_report(void);

/* APCS IPC doorbells (qcom_ipc.c): the "mboxes" entry of a node */
int	qcom_ipc_get(phandle_t node, int index, phandle_t *xref, u_int *bit);
int	qcom_ipc_kick(phandle_t xref, u_int bit);

/* SMP2P (qcom_smp2p.c): entries by the xref of their device tree node */
typedef void qcom_smp2p_cb_t(void *arg, u_int bit, bool set);
int	qcom_smp2p_in_watch(phandle_t entry, u_int bit, qcom_smp2p_cb_t *cb,
	    void *arg);
void	qcom_smp2p_in_unwatch(phandle_t entry, u_int bit);
int	qcom_smp2p_in_read(phandle_t entry, uint32_t *value);
int	qcom_smp2p_out_update(phandle_t entry, uint32_t mask, uint32_t value);

/* GLINK over SMEM (qcom_glink.c), the edges of the remote processors */
struct glink_edge;
/* a remote processor opened CHANNEL on EDGE (label: "lpass", "mpss", ...) */
typedef void qcom_glink_user_cb_t(void *arg, struct glink_edge *edge,
    const char *label, const char *channel);
int	qcom_glink_edge_start(device_t dev, phandle_t rproc, void **cookie);
void	qcom_glink_edge_stop(void *cookie);
void	qcom_glink_register(const char *channel, qcom_glink_user_cb_t *cb,
	    void *arg);
void	qcom_glink_unregister(const char *channel);
struct glink_edge *qcom_glink_find(const char *label);

#endif /* _QCOM_DSP_H_ */
