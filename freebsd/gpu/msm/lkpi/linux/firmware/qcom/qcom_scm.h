/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The DSP glue's qcom_scm_assign_mem (freebsd/dsp lkpi/linux/firmware/qcom/
 * qcom_scm.h) and Linux's PAS calls for the GPU's zap shader (mdt_loader.c,
 * adreno_gpu.c) on qcom_dsp's secure monitor calls (qcom_scm.c):
 * init_image with the metadata in physically contiguous memory, mem_setup,
 * auth_and_reset (msm_freebsd.c). Not here: the GPU's register init call
 * (a7xx only) and set_remote_state (a5xx's zap resume).
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_FIRMWARE_QCOM_QCOM_SCM_H
#define	_MSM_LKPI_LINUX_FIRMWARE_QCOM_QCOM_SCM_H

#include_next <linux/firmware/qcom/qcom_scm.h>

#include <linux/types.h>
#include <linux/err.h>

#define	QCOM_SCM_GPU_ALWAYS_EN_REQ	BIT(0)
#define	QCOM_SCM_GPU_TSENSE_EN_REQ	BIT(1)

/*
 * qcom_dsp.ko has FreeBSD's functions of these names (other arguments,
 * positive errnos): Linux's are msm.ko's own, under other names
 */
#define	qcom_scm_is_available		lkpi_qcom_scm_is_available
#define	qcom_scm_pas_init_image		lkpi_qcom_scm_pas_init_image
#define	qcom_scm_pas_metadata_release	lkpi_qcom_scm_pas_metadata_release
#define	qcom_scm_pas_mem_setup		lkpi_qcom_scm_pas_mem_setup
#define	qcom_scm_pas_auth_and_reset	lkpi_qcom_scm_pas_auth_and_reset
#define	qcom_scm_pas_shutdown		lkpi_qcom_scm_pas_shutdown
#define	qcom_scm_pas_supported		lkpi_qcom_scm_pas_supported

struct qcom_scm_pas_metadata {
	void		*ptr;
	dma_addr_t	phys;
	ssize_t		size;
};

bool	qcom_scm_is_available(void);
int	qcom_scm_pas_init_image(u32 peripheral, const void *metadata,
	    size_t size, struct qcom_scm_pas_metadata *ctx);
void	qcom_scm_pas_metadata_release(struct qcom_scm_pas_metadata *ctx);
int	qcom_scm_pas_mem_setup(u32 peripheral, phys_addr_t addr,
	    phys_addr_t size);
int	qcom_scm_pas_auth_and_reset(u32 peripheral);
int	qcom_scm_pas_shutdown(u32 peripheral);
bool	qcom_scm_pas_supported(u32 peripheral);

static inline int
qcom_scm_set_remote_state(u32 state, u32 id)
{
	return (-EOPNOTSUPP);
}

static inline int
qcom_scm_gpu_init_regs(u32 gpu_req)
{
	return (-EOPNOTSUPP);
}

#endif
