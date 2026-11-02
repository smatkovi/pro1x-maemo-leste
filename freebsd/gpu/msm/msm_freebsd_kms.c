// SPDX-License-Identifier: GPL-2.0-only
/*
 * KMS for the display Linux leaves running on the F(x)tec Pro1X
 * (freebsd/gpu/kms.md), in msm.ko's DRM device: one CRTC, one primary
 * plane, one DSI encoder and connector, so that /dev/dri/card0 offers KMS
 * and the Adreno 610 together (Xorg's modesetting driver with glamor,
 * Mesa's freedreno through GBM).
 *
 * Nothing of the display is set up here. Linux's msm leaves it running at
 * the kexec (the Pro1X kernel's device/kernel/patches/0005): DSI in video
 * mode, INTF_1's timing engine, CTL_0, LM_0, the source pipe. FreeBSD's
 * qcom_dpu (patches/0017) has turned translation off in the display's SMMU
 * context banks and pointed the pipe at the console's framebuffer. This
 * file reads that state (the mode from INTF_1's timing registers), and a
 * commit only writes the pipe's source address and stride and flushes
 * CTL_0, as qcom_dpu does once: the next frame shows the new buffer. All
 * buffers are msm's GEM objects in its VRAM carveout, physically
 * contiguous and below 4 GiB (the pipe's address register has 32 bits),
 * so their physical address is what the pipe fetches from. Without a
 * framebuffer (at load, after the last client, at unload) the pipe shows
 * the console's framebuffer again: vt keeps drawing there.
 *
 * Vblank: INTF_1's vsync interrupt (the DPU's INTR bit 27, dpu_6_3_sm6115.h)
 * through qcom_dpu's MDSS interrupt (patches/0053, msm_bsd_kms_intr_*), or
 * a poll of INTF_1's frame counter when that is not there. A page flip is
 * done at the first vblank after which CTL_0's flush bits read back clear
 * (the hardware takes a flush at the frame start, dpu_encoder_phys_vid.c's
 * vblank handler checks the same).
 *
 * Register offsets (relative to the DPU's "mdp" registers) are Linux
 * 6.12's: dpu_6_3_sm6115.h (CTL_0 0x1000, VIG0 0x4000, DMA0 0x24000,
 * INTF_1 0x6a800), dpu_hw_sspp.c, dpu_hw_ctl.c, dpu_hw_intf.c,
 * dpu_hwio.h; the DSI controller's from dsi.xml.
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_kms.h"

#include "msm_freebsd.h"

/* CTL_0 (dpu_hw_ctl.c) */
#define	CTL0			0x1000
#define	CTL_LAYER_LM0		0x000
#define	CTL_TOP			0x014
#define	CTL_FLUSH		0x018
#define	 CTL_FLUSH_MASK_CTL	BIT(17)

/* the source pipes (dpu_hw_sspp.c), their CTL flush bits (dpu_hw_ctl.c) */
#define	SSPP_SRC_SIZE		0x00
#define	SSPP_SRC_XY		0x08
#define	SSPP_OUT_SIZE		0x0c
#define	SSPP_OUT_XY		0x10
#define	SSPP_SRC0_ADDR		0x14
#define	SSPP_SRC_YSTRIDE0	0x24
#define	SSPP_SRC_FORMAT		0x30
#define	SSPP_SRC_UNPACK_PATTERN	0x34
#define	SSPP_SRC_OP_MODE	0x38

/*
 * XRGB8888 and ARGB8888, linear (mdp_format.c's INTERLEAVED_RGB_FMT with
 * BPC8A/BPC8, elements C1_B_Cb C0_G_Y C2_R_Cr C3_ALPHA, as
 * dpu_hw_sspp_setup_format() writes them); bit 8 (SRCC3_EN) is ARGB's alpha
 */
#define	FMT_XRGB8888		0x000236ffu
#define	FMT_SRCC3_EN		BIT(8)
#define	UNPACK_XRGB8888		0x03020001u

static const struct {
	const char	*name;
	uint32_t	base;
	uint32_t	flush;
} kms_pipes[] = {
	{ "VIG0",	0x4000,		BIT(0) },
	{ "DMA0",	0x24000,	BIT(11) },
};
#define	KMS_NPIPES	2

/* INTF_1 (dpu_hw_intf.c) */
#define	INTF1			0x6a800
#define	INTF_TIMING_ENGINE_EN	0x000
#define	INTF_CONFIG		0x004
#define	INTF_HSYNC_CTL		0x008
#define	INTF_VSYNC_PERIOD_F0	0x00c
#define	INTF_VSYNC_PULSE_WIDTH_F0 0x014
#define	INTF_DISPLAY_V_START_F0	0x01c
#define	INTF_DISPLAY_V_END_F0	0x024
#define	INTF_DISPLAY_HCTL	0x03c
#define	INTF_HSYNC_SKEW		0x04c
#define	INTF_POLARITY_CTL	0x050
#define	INTF_FRAME_LINE_COUNT_EN 0x0a8
#define	INTF_FRAME_COUNT	0x0ac
#define	INTF_LINE_COUNT		0x0b0

/* the DPU's top interrupt enable (dpu_hwio.h), INTF_1's bits (catalog) */
#define	MDP_INTR_EN		0x010
#define	 INTR_INTF1_UNDERRUN	BIT(26)
#define	 INTR_INTF1_VSYNC	BIT(27)

/* DSI_CTRL (dsi.xml, 4 bytes into the 6G controller's block) */
#define	DSI_CTRL		0x004
#define	 DSI_CTRL_ENABLE	BIT(0)
#define	 DSI_CTRL_VID_MODE_EN	BIT(1)
#define	 DSI_CTRL_CMD_MODE_EN	BIT(2)

/* the panel's size (panel-boe-bf060y8m-aj0.c: 68.04 x 136.08 mm) */
#define	PANEL_WIDTH_MM		68
#define	PANEL_HEIGHT_MM		136
/* its refresh: 1236 x 2192 at 162.559 MHz, exactly 60 Hz (the same file) */
#define	PANEL_HZ		60

struct msm_fbsd_kms {
	struct drm_device	*dev;
	struct drm_crtc		crtc;
	struct drm_plane	plane;
	struct drm_encoder	encoder;
	struct drm_connector	connector;
	struct drm_display_mode	mode;

	void __iomem		*mdp;
	uint64_t		mdp_size;

	/* the pipes that scan out (Linux's, the console's now) */
	int			npipes;
	uint32_t		pipe_base[KMS_NPIPES];
	uint32_t		flush;		/* their bits and CTL's */
	/* the console's scanout, as found: back to it without a client */
	uint32_t		con_addr[KMS_NPIPES];
	uint32_t		con_stride[KMS_NPIPES];
	uint32_t		con_format[KMS_NPIPES];
	bool			on_console;

	/* vblank */
	bool			irq;		/* qcom_dpu's MDSS interrupt */
	bool			hw_counter;	/* INTF_1's frame counter runs */
	uint32_t		last_frame;	/* polling: the counter seen */
	spinlock_t		intr_lock;	/* MDP_INTR_EN */
	/* a flip's event, until CTL_0 has taken the flush (event_lock) */
	struct drm_pending_vblank_event *event;
};

static struct msm_fbsd_kms *kms_sc;

static inline uint32_t
mdp_rd(struct msm_fbsd_kms *sc, uint32_t off)
{
	return (readl((char __iomem *)sc->mdp + off));
}

static inline void
mdp_wr(struct msm_fbsd_kms *sc, uint32_t off, uint32_t v)
{
	writel(v, (char __iomem *)sc->mdp + off);
}

static void
mdp_intr_set(struct msm_fbsd_kms *sc, uint32_t bits, bool on)
{
	unsigned long flags;
	uint32_t v;

	spin_lock_irqsave(&sc->intr_lock, flags);
	v = mdp_rd(sc, MDP_INTR_EN);
	mdp_wr(sc, MDP_INTR_EN, on ? v | bits : v & ~bits);
	spin_unlock_irqrestore(&sc->intr_lock, flags);
}

/* --- scanout ----------------------------------------------------------------------- */

/* every scanning pipe from addr with stride; CTL_0 flushed by the caller */
static void
kms_scanout(struct msm_fbsd_kms *sc, uint32_t addr, uint32_t stride,
    bool console)
{
	uint32_t b, v;
	int i;

	for (i = 0; i < sc->npipes; i++) {
		b = sc->pipe_base[i];
		if (console) {
			addr = sc->con_addr[i];
			stride = sc->con_stride[i];
		}
		mdp_wr(sc, b + SSPP_SRC0_ADDR, addr);
		v = mdp_rd(sc, b + SSPP_SRC_YSTRIDE0);
		mdp_wr(sc, b + SSPP_SRC_YSTRIDE0, (v & 0xffff0000u) | stride);
		/* opaque: a client's ARGB8888 shown as XRGB8888 */
		mdp_wr(sc, b + SSPP_SRC_FORMAT, console ? sc->con_format[i] :
		    sc->con_format[i] & ~FMT_SRCC3_EN);
	}
	sc->on_console = console;
}

static void
kms_flush(struct msm_fbsd_kms *sc)
{
	mdp_wr(sc, CTL0 + CTL_FLUSH, sc->flush);
}

static bool
kms_flush_pending(struct msm_fbsd_kms *sc)
{
	return ((mdp_rd(sc, CTL0 + CTL_FLUSH) & sc->flush) != 0);
}

/* sleeping: up to two frames for CTL_0 to take a flush */
static void
kms_flush_wait(struct msm_fbsd_kms *sc)
{
	int i;

	for (i = 0; i < 40 && kms_flush_pending(sc); i++)
		usleep_range(1000, 1500);
	if (kms_flush_pending(sc))
		DRM_DEV_INFO(sc->dev->dev, "kms: flush %#x still pending\n",
		    mdp_rd(sc, CTL0 + CTL_FLUSH));
}

/* --- vblank -------------------------------------------------------------------------- */

static void
kms_vblank(struct msm_fbsd_kms *sc)
{
	struct drm_device *dev = sc->dev;
	struct drm_pending_vblank_event *ev = NULL;
	unsigned long flags;

	msm_bsd_kms_count(MSM_KMS_VBLANKS);
	drm_crtc_handle_vblank(&sc->crtc);
	spin_lock_irqsave(&dev->event_lock, flags);
	if (sc->event != NULL) {
		if (!kms_flush_pending(sc)) {
			ev = sc->event;
			sc->event = NULL;
			drm_crtc_send_vblank_event(&sc->crtc, ev);
		} else
			msm_bsd_kms_count(MSM_KMS_FLIPS_LATE);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
	if (ev != NULL) {
		msm_bsd_kms_count(MSM_KMS_FLIPS);
		drm_crtc_vblank_put(&sc->crtc);
	}
}

/* qcom_dpu's MDSS interrupt thread (qcom_dpu_mdp_intr, patches/0053) */
static void
kms_mdp_intr(void *arg, uint32_t intr, uint32_t intr2)
{
	struct msm_fbsd_kms *sc = arg;

	if (intr & INTR_INTF1_UNDERRUN)
		msm_bsd_kms_count(MSM_KMS_UNDERRUNS);
	if (intr & INTR_INTF1_VSYNC)
		kms_vblank(sc);
}

/* without the interrupt: a callout, a vblank per new frame counted */
static void
kms_poll(void *arg)
{
	struct msm_fbsd_kms *sc = arg;
	uint32_t n;

	if (sc->hw_counter) {
		n = mdp_rd(sc, INTF1 + INTF_FRAME_COUNT);
		if (n == sc->last_frame)
			return;
		sc->last_frame = n;
	}
	kms_vblank(sc);
}

static int
kms_enable_vblank(struct drm_crtc *crtc)
{
	struct msm_fbsd_kms *sc = kms_sc;

	if (sc->irq) {
		mdp_intr_set(sc, INTR_INTF1_VSYNC, true);
	} else {
		sc->last_frame = sc->hw_counter ?
		    mdp_rd(sc, INTF1 + INTF_FRAME_COUNT) : 0;
		/* a quarter frame with the counter, else a frame */
		msm_bsd_kms_poll(kms_poll, sc, sc->hw_counter ?
		    1000000 / PANEL_HZ / 4 : 1000000 / PANEL_HZ);
	}
	return (0);
}

static void
kms_disable_vblank(struct drm_crtc *crtc)
{
	struct msm_fbsd_kms *sc = kms_sc;

	if (sc->irq)
		mdp_intr_set(sc, INTR_INTF1_VSYNC, false);
	else
		msm_bsd_kms_poll(NULL, NULL, 0);
}

static u32
kms_get_vblank_counter(struct drm_crtc *crtc)
{
	struct msm_fbsd_kms *sc = kms_sc;

	/* only called with dev->max_vblank_count set: the counter runs */
	return (mdp_rd(sc, INTF1 + INTF_FRAME_COUNT));
}

/* --- the plane ------------------------------------------------------------------------- */

static const uint32_t kms_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const uint64_t kms_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static bool
kms_format_mod_supported(struct drm_plane *plane, uint32_t format,
    uint64_t modifier)
{
	return (modifier == DRM_FORMAT_MOD_LINEAR);
}

static int
kms_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
	    plane);
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb = new->fb;
	int ret;

	if (new->crtc == NULL)
		return (0);
	crtc_state = drm_atomic_get_crtc_state(state, new->crtc);
	if (IS_ERR(crtc_state))
		return (PTR_ERR(crtc_state));
	/* the whole mode, no scaling: the pipe's geometry stays Linux's */
	ret = drm_atomic_helper_check_plane_state(new, crtc_state,
	    DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false, true);
	if (ret != 0 || !new->visible)
		return (ret);
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR || fb->pitches[0] > 0xffff ||
	    (fb->pitches[0] & 7) != 0)
		return (-EINVAL);
	return (0);
}

static int
kms_prepare_fb(struct drm_plane *plane, struct drm_plane_state *new)
{
	struct drm_gem_object *obj;
	uint64_t iova;
	int ret;

	if (new->fb == NULL)
		return (0);
	ret = drm_gem_plane_helper_prepare_fb(plane, new);
	if (ret != 0)
		return (ret);
	obj = new->fb->obj[0];
	/* the carveout's objects: their iova is their physical address */
	ret = msm_gem_get_and_pin_iova(obj, NULL, &iova);
	if (ret != 0)
		return (ret);
	if (iova + obj->size > (1ULL << 32)) {
		msm_gem_unpin_iova(obj, NULL);
		DRM_DEV_ERROR(plane->dev->dev, "kms: buffer at %#llx, above "
		    "the pipe's 32-bit address\n", (unsigned long long)iova);
		return (-EINVAL);
	}
	return (0);
}

static void
kms_cleanup_fb(struct drm_plane *plane, struct drm_plane_state *old)
{
	if (old->fb != NULL)
		msm_gem_unpin_iova(old->fb->obj[0], NULL);
}

static void
kms_plane_atomic_update(struct drm_plane *plane,
    struct drm_atomic_state *state)
{
	struct msm_fbsd_kms *sc = kms_sc;
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
	    plane);
	struct drm_framebuffer *fb = new->fb;
	uint64_t iova;

	if (fb == NULL || !new->visible ||
	    msm_gem_get_iova(fb->obj[0], NULL, &iova) != 0) {
		kms_scanout(sc, 0, 0, true);
		return;
	}
	iova += fb->offsets[0] + (uint64_t)(new->src.y1 >> 16) *
	    fb->pitches[0] + (uint64_t)(new->src.x1 >> 16) * fb->format->cpp[0];
	kms_scanout(sc, (uint32_t)iova, fb->pitches[0], false);
}

static const struct drm_plane_helper_funcs kms_plane_helper_funcs = {
	.prepare_fb	= kms_prepare_fb,
	.cleanup_fb	= kms_cleanup_fb,
	.atomic_check	= kms_plane_atomic_check,
	.atomic_update	= kms_plane_atomic_update,
};

static const struct drm_plane_funcs kms_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
	.format_mod_supported	= kms_format_mod_supported,
};

/* --- the CRTC --------------------------------------------------------------------------- */

static enum drm_mode_status
kms_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	return (drm_mode_equal(mode, &kms_sc->mode) ? MODE_OK : MODE_BAD);
}

static int
kms_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);

	if (!cs->enable)
		return (0);
	/* the panel's mode, and the primary plane on it */
	if (!drm_mode_equal(&cs->mode, &kms_sc->mode) ||
	    !(cs->plane_mask & drm_plane_mask(crtc->primary)))
		return (-EINVAL);
	return (0);
}

static void
kms_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	/* the display runs: only vblank */
	drm_crtc_vblank_on(crtc);
}

static void
kms_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct msm_fbsd_kms *sc = kms_sc;
	struct drm_pending_vblank_event *ev;
	unsigned long flags;

	/* the console's framebuffer, taken before the client's goes away */
	kms_scanout(sc, 0, 0, true);
	kms_flush(sc);
	kms_flush_wait(sc);
	drm_crtc_vblank_off(crtc);
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	ev = crtc->state->event;
	crtc->state->event = NULL;
	if (ev != NULL)
		drm_crtc_send_vblank_event(crtc, ev);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void
kms_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct msm_fbsd_kms *sc = kms_sc;
	struct drm_pending_vblank_event *ev = crtc->state->event;
	unsigned long flags;

	if (!crtc->state->active) {
		/* off: atomic_disable has sent its event */
		return;
	}
	/* the flush first, then the event: a vblank in between only makes
	 * the event a frame late, never early */
	kms_flush(sc);
	if (ev == NULL)
		return;
	crtc->state->event = NULL;
	if (drm_crtc_vblank_get(crtc) != 0) {
		/* no vblank: the event at once, the flip a frame later */
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, ev);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		return;
	}
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	WARN_ON(sc->event != NULL);
	sc->event = ev;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static const struct drm_crtc_helper_funcs kms_crtc_helper_funcs = {
	.mode_valid	= kms_crtc_mode_valid,
	.atomic_check	= kms_crtc_atomic_check,
	.atomic_flush	= kms_crtc_atomic_flush,
	.atomic_enable	= kms_crtc_atomic_enable,
	.atomic_disable	= kms_crtc_atomic_disable,
};

static const struct drm_crtc_funcs kms_crtc_funcs = {
	.reset			= drm_atomic_helper_crtc_reset,
	.destroy		= drm_crtc_cleanup,
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= kms_enable_vblank,
	.disable_vblank		= kms_disable_vblank,
	.get_vblank_counter	= kms_get_vblank_counter,
};

/* --- the encoder and connector: the DSI panel, one mode ----------------------------------- */

static const struct drm_encoder_funcs kms_encoder_funcs = {
	.destroy	= drm_encoder_cleanup,
};

static int
kms_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &kms_sc->mode);
	if (mode == NULL)
		return (0);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = PANEL_WIDTH_MM;
	connector->display_info.height_mm = PANEL_HEIGHT_MM;
	return (1);
}

static const struct drm_connector_helper_funcs kms_connector_helper_funcs = {
	.get_modes	= kms_connector_get_modes,
};

static enum drm_connector_status
kms_connector_detect(struct drm_connector *connector, bool force)
{
	return (connector_status_connected);
}

static const struct drm_connector_funcs kms_connector_funcs = {
	.detect			= kms_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

/* --- framebuffers ------------------------------------------------------------------------- */

static void
kms_fb_destroy(struct drm_framebuffer *fb)
{
	drm_gem_object_put(fb->obj[0]);
	drm_framebuffer_cleanup(fb);
	kfree(fb);
}

static const struct drm_framebuffer_funcs kms_fb_funcs = {
	.destroy	= kms_fb_destroy,
	.create_handle	= drm_gem_fb_create_handle,
};

static struct drm_framebuffer *
kms_fb_create(struct drm_device *dev, struct drm_file *file,
    const struct drm_mode_fb_cmd2 *cmd)
{
	const struct drm_format_info *info;
	struct drm_gem_object *obj;
	struct drm_framebuffer *fb;
	uint64_t min_size;
	int ret;

	if (cmd->pixel_format != DRM_FORMAT_XRGB8888 &&
	    cmd->pixel_format != DRM_FORMAT_ARGB8888)
		return (ERR_PTR(-EINVAL));
	if ((cmd->flags & DRM_MODE_FB_MODIFIERS) &&
	    cmd->modifier[0] != DRM_FORMAT_MOD_LINEAR)
		return (ERR_PTR(-EINVAL));
	info = drm_get_format_info(dev, cmd);
	if (info == NULL || info->num_planes != 1)
		return (ERR_PTR(-EINVAL));
	obj = drm_gem_object_lookup(file, cmd->handles[0]);
	if (obj == NULL)
		return (ERR_PTR(-ENOENT));
	min_size = (uint64_t)(cmd->height - 1) * cmd->pitches[0] +
	    (uint64_t)cmd->width * info->cpp[0] + cmd->offsets[0];
	if (cmd->pitches[0] < cmd->width * info->cpp[0] ||
	    obj->size < min_size) {
		ret = -EINVAL;
		goto put;
	}
	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (fb == NULL) {
		ret = -ENOMEM;
		goto put;
	}
	drm_helper_mode_fill_fb_struct(dev, fb, cmd);
	fb->obj[0] = obj;
	ret = drm_framebuffer_init(dev, fb, &kms_fb_funcs);
	if (ret != 0) {
		kfree(fb);
		goto put;
	}
	return (fb);
put:
	drm_gem_object_put(obj);
	return (ERR_PTR(ret));
}

/*
 * The commit's tail as drm_atomic_helper_commit_tail_rpm() (the CRTC's
 * vblank on before its planes are flushed), but waiting for the flips (the
 * events kms_vblank() sends once CTL_0 took the flush) instead of for a
 * vblank: a framebuffer is unpinned only once the pipe fetches from the
 * next
 */
static void
kms_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, old_state);
	drm_atomic_helper_commit_modeset_enables(dev, old_state);
	drm_atomic_helper_commit_planes(dev, old_state,
	    DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_fake_vblank(old_state);
	drm_atomic_helper_commit_hw_done(old_state);
	drm_atomic_helper_wait_for_flip_done(dev, old_state);
	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static const struct drm_mode_config_funcs kms_mode_config_funcs = {
	.fb_create	= kms_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs kms_mode_config_helpers = {
	.atomic_commit_tail = kms_commit_tail,
};

/* --- what Linux left running ---------------------------------------------------------------- */

/*
 * The mode from INTF_1's timing registers, as dpu_hw_intf_setup_timing_engine()
 * computed them from Linux's mode (drm_mode_to_intf_timing_params(): no
 * borders, no skew for DSI, sync polarities 0); the clock for the panel's
 * 60 Hz
 */
static int
kms_read_mode(struct msm_fbsd_kms *sc, struct drm_display_mode *m)
{
	uint32_t hctl, hperiod, hpw, hstart, hend, skew, vperiod, vpw, vstart,
	    vend;

	hctl = mdp_rd(sc, INTF1 + INTF_HSYNC_CTL);
	hperiod = hctl >> 16;
	hpw = hctl & 0xffff;
	hstart = mdp_rd(sc, INTF1 + INTF_DISPLAY_HCTL) & 0xffff;
	hend = mdp_rd(sc, INTF1 + INTF_DISPLAY_HCTL) >> 16;
	skew = mdp_rd(sc, INTF1 + INTF_HSYNC_SKEW);
	if (hperiod == 0 || hend <= hstart || hend >= hperiod || hstart < hpw)
		return (-EINVAL);
	vperiod = mdp_rd(sc, INTF1 + INTF_VSYNC_PERIOD_F0) / hperiod;
	vpw = mdp_rd(sc, INTF1 + INTF_VSYNC_PULSE_WIDTH_F0) / hperiod;
	vstart = (mdp_rd(sc, INTF1 + INTF_DISPLAY_V_START_F0) - skew) / hperiod;
	vend = (mdp_rd(sc, INTF1 + INTF_DISPLAY_V_END_F0) + 1 - skew) / hperiod;
	if (vend <= vstart || vend > vperiod || vstart < vpw)
		return (-EINVAL);

	memset(m, 0, sizeof(*m));
	m->hdisplay = hend - hstart + 1;
	m->hsync_start = m->hdisplay + (hperiod - hend - 1);
	m->hsync_end = m->hsync_start + hpw;
	m->htotal = hperiod;
	m->vdisplay = vend - vstart;
	m->vsync_start = m->vdisplay + (vperiod - vend);
	m->vsync_end = m->vsync_start + vpw;
	m->vtotal = vperiod;
	m->clock = DIV_ROUND_CLOSEST(m->htotal * m->vtotal * PANEL_HZ, 1000);
	m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	m->width_mm = PANEL_WIDTH_MM;
	m->height_mm = PANEL_HEIGHT_MM;
	drm_mode_set_name(m);
	return (0);
}

static int
kms_check_dsi(struct drm_device *dev)
{
	void __iomem *dsi;
	uintptr_t node;
	uint64_t pa, size;
	uint32_t ctrl;

	node = msm_bsd_of_find_compatible("qcom,mdss-dsi-ctrl");
	if (node == 0 || !msm_bsd_of_is_available(node) ||
	    msm_bsd_of_reg(node, 0, &pa, &size) != 0)
		return (-ENODEV);
	dsi = ioremap(pa, size);
	if (dsi == NULL)
		return (-ENOMEM);
	ctrl = readl((char __iomem *)dsi + DSI_CTRL);
	iounmap(dsi);
	if ((ctrl & (DSI_CTRL_ENABLE | DSI_CTRL_VID_MODE_EN)) !=
	    (DSI_CTRL_ENABLE | DSI_CTRL_VID_MODE_EN)) {
		/* command mode would need a frame transfer per update
		 * (CTL_START, PINGPONG_0's done): not written */
		DRM_DEV_INFO(dev->dev, "kms: DSI_CTRL %#x: not on in video "
		    "mode, no KMS\n", ctrl);
		return (-ENODEV);
	}
	DRM_DEV_INFO(dev->dev, "kms: DSI at %#llx in video mode (DSI_CTRL "
	    "%#x)\n", (unsigned long long)pa, ctrl);
	return (0);
}

/* the scanning pipes, checked against the console's framebuffer */
static int
kms_find_pipes(struct msm_fbsd_kms *sc)
{
	struct drm_device *dev = sc->dev;
	uintptr_t fbnode;
	uint64_t fb_pa = 0, fb_size;
	uint32_t b, addr, fmt, unpack, size;
	unsigned int i;

	fbnode = msm_bsd_of_find_compatible("simple-framebuffer");
	if (fbnode != 0)
		msm_bsd_of_reg(fbnode, 0, &fb_pa, &fb_size);
	sc->flush = CTL_FLUSH_MASK_CTL;
	for (i = 0; i < KMS_NPIPES; i++) {
		b = kms_pipes[i].base;
		addr = mdp_rd(sc, b + SSPP_SRC0_ADDR);
		size = mdp_rd(sc, b + SSPP_SRC_SIZE);
		fmt = mdp_rd(sc, b + SSPP_SRC_FORMAT);
		unpack = mdp_rd(sc, b + SSPP_SRC_UNPACK_PATTERN);
		DRM_DEV_INFO(dev->dev, "kms: %s: source %#x, %ux%u at %u,%u, "
		    "out %ux%u at %u,%u, stride %u, format %#x, unpack %#x, "
		    "op mode %#x\n", kms_pipes[i].name, addr, size & 0xffff,
		    size >> 16, mdp_rd(sc, b + SSPP_SRC_XY) & 0xffff,
		    mdp_rd(sc, b + SSPP_SRC_XY) >> 16,
		    mdp_rd(sc, b + SSPP_OUT_SIZE) & 0xffff,
		    mdp_rd(sc, b + SSPP_OUT_SIZE) >> 16,
		    mdp_rd(sc, b + SSPP_OUT_XY) & 0xffff,
		    mdp_rd(sc, b + SSPP_OUT_XY) >> 16,
		    mdp_rd(sc, b + SSPP_SRC_YSTRIDE0) & 0xffff, fmt, unpack,
		    mdp_rd(sc, b + SSPP_SRC_OP_MODE));
		if (addr == 0)
			continue;
		/* qcom_dpu points every scanning pipe at the console's
		 * framebuffer after it turned the SMMU's translation off */
		if (fb_pa == 0 || addr != (uint32_t)fb_pa) {
			DRM_DEV_INFO(dev->dev, "kms: %s does not show the "
			    "console's framebuffer (%#llx): qcom_dpu did not "
			    "take the display over, no KMS\n",
			    kms_pipes[i].name, (unsigned long long)fb_pa);
			return (-ENODEV);
		}
		if ((fmt & ~FMT_SRCC3_EN) != FMT_XRGB8888 ||
		    unpack != UNPACK_XRGB8888 ||
		    (size & 0xffff) != sc->mode.hdisplay ||
		    (size >> 16) != sc->mode.vdisplay) {
			DRM_DEV_INFO(dev->dev, "kms: %s is not a linear "
			    "XRGB8888 pipe of %ux%u, no KMS\n",
			    kms_pipes[i].name, sc->mode.hdisplay,
			    sc->mode.vdisplay);
			return (-ENODEV);
		}
		sc->pipe_base[sc->npipes] = b;
		sc->con_addr[sc->npipes] = addr;
		sc->con_stride[sc->npipes] = mdp_rd(sc, b + SSPP_SRC_YSTRIDE0) &
		    0xffff;
		sc->con_format[sc->npipes] = fmt;
		sc->npipes++;
		sc->flush |= kms_pipes[i].flush;
	}
	if (sc->npipes == 0) {
		DRM_DEV_INFO(dev->dev, "kms: no pipe scans out, no KMS\n");
		return (-ENODEV);
	}
	sc->on_console = true;
	return (0);
}

/* --- set-up and teardown -------------------------------------------------------------------- */

/* the interrupt, the poll and the registers; sc itself goes with the device */
static void
kms_release(struct msm_fbsd_kms *sc)
{
	if (sc->irq)
		mdp_intr_set(sc, INTR_INTF1_VSYNC | INTR_INTF1_UNDERRUN, false);
	/* waits for a handler that runs */
	msm_bsd_kms_intr_deregister();
	msm_bsd_kms_poll(NULL, NULL, 0);
	msm_bsd_kms_poll_drain();
	if (sc->mdp != NULL)
		iounmap(sc->mdp);
	sc->mdp = NULL;
}

int
msm_freebsd_kms_init(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_fbsd_kms *sc;
	struct drm_mode_config *mc;
	uintptr_t node;
	uint64_t pa, size, fbsize;
	uint32_t f0, f1;
	char info[160];
	int ret;

	if (!msm_bsd_kms_enabled())
		return (-ENODEV);
	node = msm_bsd_of_find_compatible("qcom,sm6115-dpu");
	if (node == 0 || msm_bsd_of_reg(node, 0, &pa, &size) != 0) {
		DRM_DEV_INFO(dev->dev, "kms: no DPU in the device tree\n");
		return (-ENODEV);
	}
	if ((ret = kms_check_dsi(dev)) != 0)
		return (ret);
	/* with the device: its CRTC, plane, ... stay in the mode config until
	 * the device's release, even when KMS is given up half way */
	sc = drmm_kzalloc(dev, sizeof(*sc), GFP_KERNEL);
	if (sc == NULL)
		return (-ENOMEM);
	sc->dev = dev;
	spin_lock_init(&sc->intr_lock);
	sc->mdp = ioremap(pa, size);
	sc->mdp_size = size;
	if (sc->mdp == NULL) {
		ret = -ENOMEM;
		goto fail;
	}
	if (!(mdp_rd(sc, INTF1 + INTF_TIMING_ENGINE_EN) & 1)) {
		DRM_DEV_INFO(dev->dev, "kms: INTF_1's timing engine is off: "
		    "the display does not run, no KMS\n");
		ret = -ENODEV;
		goto fail;
	}
	if ((ret = kms_read_mode(sc, &sc->mode)) != 0) {
		DRM_DEV_INFO(dev->dev, "kms: INTF_1's timing (HSYNC_CTL %#x, "
		    "DISPLAY_HCTL %#x) makes no mode, no KMS\n",
		    mdp_rd(sc, INTF1 + INTF_HSYNC_CTL),
		    mdp_rd(sc, INTF1 + INTF_DISPLAY_HCTL));
		goto fail;
	}
	DRM_DEV_INFO(dev->dev, "kms: INTF_1: " DRM_MODE_FMT ", INTF_CONFIG "
	    "%#x, polarity %#x, CTL_0 layer %#x top %#x\n",
	    DRM_MODE_ARG(&sc->mode), mdp_rd(sc, INTF1 + INTF_CONFIG),
	    mdp_rd(sc, INTF1 + INTF_POLARITY_CTL), mdp_rd(sc, CTL0 +
	    CTL_LAYER_LM0), mdp_rd(sc, CTL0 + CTL_TOP));
	if ((ret = kms_find_pipes(sc)) != 0)
		goto fail;

	/* the carveout: below 4 GiB, room for a few screens */
	fbsize = (uint64_t)ALIGN(sc->mode.hdisplay, 32) * 4 * sc->mode.vdisplay;
	if (priv->vram.paddr == 0 ||
	    (uint64_t)priv->vram.paddr + priv->vram.size > (1ULL << 32)) {
		DRM_DEV_INFO(dev->dev, "kms: VRAM %#llx+%#lx is not below "
		    "4 GiB, no KMS\n", (unsigned long long)priv->vram.paddr,
		    priv->vram.size);
		ret = -ENODEV;
		goto fail;
	}
	if (priv->vram.size < 4 * fbsize)
		DRM_DEV_INFO(dev->dev, "kms: VRAM of %lu MiB holds fewer than "
		    "4 screens of %llu KiB (hw.msm.vram)\n",
		    priv->vram.size >> 20, (unsigned long long)fbsize >> 10);

	/* INTF_1's frame counter as the vblank counter, if it counts */
	f0 = mdp_rd(sc, INTF1 + INTF_FRAME_COUNT);
	msleep(3 * 1000 / PANEL_HZ);
	f1 = mdp_rd(sc, INTF1 + INTF_FRAME_COUNT);
	sc->hw_counter = f1 != f0;
	DRM_DEV_INFO(dev->dev, "kms: INTF_1 frame counter %u -> %u in %u ms "
	    "(FRAME_LINE_COUNT_EN %#x, line %u)%s\n", f0, f1, 3 * 1000 /
	    PANEL_HZ, mdp_rd(sc, INTF1 + INTF_FRAME_LINE_COUNT_EN),
	    mdp_rd(sc, INTF1 + INTF_LINE_COUNT), sc->hw_counter ? "" :
	    ": not used");

	kms_sc = sc;
	ret = msm_bsd_kms_intr_register(kms_mdp_intr, sc);
	if (ret < 0)
		goto fail_sc;
	sc->irq = ret == 1;
	if (sc->irq)
		mdp_intr_set(sc, INTR_INTF1_UNDERRUN, true);

	ret = drmm_mode_config_init(dev);
	if (ret != 0)
		goto fail_sc;
	mc = &dev->mode_config;
	mc->min_width = sc->mode.hdisplay;
	mc->min_height = sc->mode.vdisplay;
	/* the X screen's front buffer, turned, too (2160 x 1080) */
	mc->max_width = 4096;
	mc->max_height = 4096;
	mc->preferred_depth = 24;
	mc->funcs = &kms_mode_config_funcs;
	mc->helper_private = &kms_mode_config_helpers;

	ret = drm_universal_plane_init(dev, &sc->plane, 1, &kms_plane_funcs,
	    kms_formats, ARRAY_SIZE(kms_formats), kms_modifiers,
	    DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret != 0)
		goto fail_sc;
	drm_plane_helper_add(&sc->plane, &kms_plane_helper_funcs);
	ret = drm_crtc_init_with_planes(dev, &sc->crtc, &sc->plane, NULL,
	    &kms_crtc_funcs, NULL);
	if (ret != 0)
		goto fail_sc;
	drm_crtc_helper_add(&sc->crtc, &kms_crtc_helper_funcs);
	ret = drm_encoder_init(dev, &sc->encoder, &kms_encoder_funcs,
	    DRM_MODE_ENCODER_DSI, NULL);
	if (ret != 0)
		goto fail_sc;
	sc->encoder.possible_crtcs = drm_crtc_mask(&sc->crtc);
	ret = drm_connector_init(dev, &sc->connector, &kms_connector_funcs,
	    DRM_MODE_CONNECTOR_DSI);
	if (ret != 0)
		goto fail_sc;
	drm_connector_helper_add(&sc->connector, &kms_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&sc->connector, &sc->encoder);
	if (ret != 0)
		goto fail_sc;
	ret = drm_vblank_init(dev, 1);
	if (ret != 0)
		goto fail_sc;
	dev->max_vblank_count = sc->hw_counter ? 0xffffffff : 0;
	drm_mode_config_reset(dev);

	snprintf(info, sizeof(info), "%s, %d pipe%s (%s%s), flush %#x, vblank "
	    "%s, %s counter", sc->mode.name, sc->npipes, sc->npipes > 1 ?
	    "s" : "", kms_pipes[0].base == sc->pipe_base[0] ? "VIG0" : "DMA0",
	    sc->npipes > 1 ? " DMA0" : "", sc->flush, sc->irq ?
	    "INTF_1 vsync interrupt" : "poll", sc->hw_counter ? "frame" :
	    "software");
	msm_bsd_kms_set_info(info);
	DRM_DEV_INFO(dev->dev, "kms: %s\n", info);
	return (0);

fail_sc:
	kms_sc = NULL;
fail:
	DRM_DEV_INFO(dev->dev, "kms: not set up (%d): headless\n", ret);
	kms_release(sc);
	return (ret);
}

void
msm_freebsd_kms_fini(struct drm_device *dev)
{
	struct msm_fbsd_kms *sc = kms_sc;

	if (sc == NULL || sc->dev != dev)
		return;
	/* off: the console's framebuffer, events sent */
	drm_atomic_helper_shutdown(dev);
	if (!sc->on_console) {
		kms_scanout(sc, 0, 0, true);
		kms_flush(sc);
		kms_flush_wait(sc);
	}
	kms_sc = NULL;
	kms_release(sc);
}
