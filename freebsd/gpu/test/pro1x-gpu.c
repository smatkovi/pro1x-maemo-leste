/*
 * The device runs' small program for msm.ko (freebsd/gpu/msm-port.md,
 * kms.md; pro1x-gputest runs it): what the GPU and the display say, through
 * libdrm and msm's ioctls (Linux v6.12's uapi msm_drm.h).
 *
 *   pro1x-gpu info [DEV]        driver name and version, bus id, caps
 *   pro1x-gpu param [DEV]       msm's GET_PARAMs (gpu2, gpu3): GPU_ID,
 *                               CHIP_ID, MAX_FREQ, TIMESTAMP twice, ...
 *   pro1x-gpu submit [DEV]      a submit (gpu4): CP_MEM_WRITE of a pattern
 *                               into a buffer, the fence waited for (1 s),
 *                               the pattern read back
 *   pro1x-gpu kms [SECONDS]     KMS (kms1, kms2) on /dev/dri/card0: the
 *                               connector, its mode, the plane's formats; a
 *                               test pattern in two dumb buffers flipped for
 *                               SECONDS (default 10) with a bar moving down,
 *                               the flip events and vblanks counted
 *
 * DEV defaults to /dev/dri/renderD128 (card0 for kms). Each check prints
 * "ok: ..." or "FAIL: ..."; the exit status is the number of failures.
 */
#include <sys/mman.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "msm_drm.h"

static int nfail;

static void
ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
ok(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("ok: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

static void
fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("FAIL: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	nfail++;
}

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

static int
open_dev(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		fail("open %s: %s", path, strerror(errno));
	return (fd);
}

/* --- info ---------------------------------------------------------------- */

static int
cmd_info(const char *path)
{
	static const struct { uint64_t cap; const char *name; } caps[] = {
		{ DRM_CAP_DUMB_BUFFER, "DUMB_BUFFER" },
		{ DRM_CAP_PRIME, "PRIME" },
		{ DRM_CAP_SYNCOBJ, "SYNCOBJ" },
		{ DRM_CAP_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC" },
		{ DRM_CAP_ADDFB2_MODIFIERS, "ADDFB2_MODIFIERS" },
		{ DRM_CAP_CRTC_IN_VBLANK_EVENT, "CRTC_IN_VBLANK_EVENT" },
	};
	drmVersionPtr v;
	uint64_t val;
	char *busid;
	unsigned int i;
	int fd;

	if ((fd = open_dev(path)) < 0)
		return (nfail);
	v = drmGetVersion(fd);
	if (v == NULL) {
		fail("%s: no DRM version: %s", path, strerror(errno));
		return (nfail);
	}
	ok("%s: driver %s %d.%d.%d (%s, %s)", path, v->name,
	    v->version_major, v->version_minor, v->version_patchlevel,
	    v->desc, v->date);
	if (strcmp(v->name, "msm") != 0)
		fail("driver %s, not msm", v->name);
	drmFreeVersion(v);
	busid = drmGetBusid(fd);
	printf("busid: %s\n", busid != NULL ? busid : "(none)");
	drmFreeBusid(busid);
	for (i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
		if (drmGetCap(fd, caps[i].cap, &val) == 0)
			printf("cap %s: %ju\n", caps[i].name, (uintmax_t)val);
		else
			printf("cap %s: %s\n", caps[i].name, strerror(errno));
	close(fd);
	return (nfail);
}

/* --- msm's parameters ----------------------------------------------------- */

static int
get_param(int fd, uint32_t param, uint64_t *val)
{
	struct drm_msm_param p;

	memset(&p, 0, sizeof(p));
	p.pipe = MSM_PIPE_3D0;
	p.param = param;
	if (drmCommandWriteRead(fd, DRM_MSM_GET_PARAM, &p, sizeof(p)) != 0)
		return (-errno);
	*val = p.value;
	return (0);
}

static int
cmd_param(const char *path)
{
	static const struct { uint32_t p; const char *name; } params[] = {
		{ MSM_PARAM_GPU_ID, "GPU_ID" },
		{ MSM_PARAM_CHIP_ID, "CHIP_ID" },
		{ MSM_PARAM_MAX_FREQ, "MAX_FREQ" },
		{ MSM_PARAM_GMEM_SIZE, "GMEM_SIZE" },
		{ MSM_PARAM_GMEM_BASE, "GMEM_BASE" },
		{ MSM_PARAM_NR_RINGS, "NR_RINGS" },
		{ MSM_PARAM_FAULTS, "FAULTS" },
		{ MSM_PARAM_SUSPENDS, "SUSPENDS" },
		{ MSM_PARAM_VA_START, "VA_START" },
		{ MSM_PARAM_VA_SIZE, "VA_SIZE" },
		{ MSM_PARAM_HIGHEST_BANK_BIT, "HIGHEST_BANK_BIT" },
	};
	uint64_t v, t0, t1;
	unsigned int i;
	int fd, r;

	if ((fd = open_dev(path)) < 0)
		return (nfail);
	for (i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		r = get_param(fd, params[i].p, &v);
		if (r == 0)
			printf("param %s: %#jx (%ju)\n", params[i].name,
			    (uintmax_t)v, (uintmax_t)v);
		else
			printf("param %s: %s\n", params[i].name, strerror(-r));
	}
	if (get_param(fd, MSM_PARAM_GPU_ID, &v) == 0 && v == 610)
		ok("GPU_ID 610");
	else
		fail("GPU_ID not 610");
	if (get_param(fd, MSM_PARAM_CHIP_ID, &v) == 0 &&
	    (v & 0xffff0000) == 0x06010000)
		ok("CHIP_ID %#jx (6.1.0.x)", (uintmax_t)v);
	else
		fail("CHIP_ID not 0x0601xxxx");
	/* the GPU's always-on counter: rising */
	if (get_param(fd, MSM_PARAM_TIMESTAMP, &t0) == 0) {
		usleep(100000);
		if (get_param(fd, MSM_PARAM_TIMESTAMP, &t1) == 0 && t1 > t0)
			ok("TIMESTAMP %ju -> %ju in 0.1 s", (uintmax_t)t0,
			    (uintmax_t)t1);
		else
			fail("TIMESTAMP does not rise (%ju)", (uintmax_t)t0);
	} else
		fail("TIMESTAMP: %s", strerror(errno));
	close(fd);
	return (nfail);
}

/* --- a submit ------------------------------------------------------------- */

static uint32_t
pm4_parity(uint32_t v)
{
	return ((0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^
	    (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1);
}

/* a type 7 packet header, as adreno_gpu.h's OUT_PKT7 */
static uint32_t
pkt7(uint32_t opcode, uint32_t cnt)
{
	return (0x70000000u | cnt | (pm4_parity(cnt) << 15) |
	    ((opcode & 0x7f) << 16) | (pm4_parity(opcode) << 23));
}

#define	CP_MEM_WRITE	61	/* adreno_pm4.xml */
#define	CP_NOP		16

struct bo {
	uint32_t	handle;
	uint64_t	iova;
	uint32_t	*map;
	size_t		size;
};

static int
bo_new(int fd, struct bo *bo, size_t size)
{
	struct drm_msm_gem_new req;
	struct drm_msm_gem_info info;
	uint64_t off;

	memset(bo, 0, sizeof(*bo));
	memset(&req, 0, sizeof(req));
	req.size = size;
	req.flags = MSM_BO_WC;
	if (drmCommandWriteRead(fd, DRM_MSM_GEM_NEW, &req, sizeof(req)) != 0) {
		fail("GEM_NEW %zu: %s", size, strerror(errno));
		return (-1);
	}
	bo->handle = req.handle;
	bo->size = size;
	memset(&info, 0, sizeof(info));
	info.handle = bo->handle;
	info.info = MSM_INFO_GET_IOVA;
	if (drmCommandWriteRead(fd, DRM_MSM_GEM_INFO, &info,
	    sizeof(info)) != 0) {
		fail("GEM_INFO GET_IOVA: %s", strerror(errno));
		return (-1);
	}
	bo->iova = info.value;
	memset(&info, 0, sizeof(info));
	info.handle = bo->handle;
	info.info = MSM_INFO_GET_OFFSET;
	if (drmCommandWriteRead(fd, DRM_MSM_GEM_INFO, &info,
	    sizeof(info)) != 0) {
		fail("GEM_INFO GET_OFFSET: %s", strerror(errno));
		return (-1);
	}
	off = info.value;
	bo->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)off);
	if (bo->map == MAP_FAILED) {
		fail("mmap of a GEM object: %s", strerror(errno));
		bo->map = NULL;
		return (-1);
	}
	return (0);
}

static int
cmd_submit(const char *path)
{
	struct drm_msm_gem_submit_bo bos[2];
	struct drm_msm_gem_submit_cmd cmd;
	struct drm_msm_gem_submit req;
	struct drm_msm_wait_fence wait;
	struct bo cs, res;
	struct timespec ts;
	double t0;
	uint32_t *p;
	int fd, i, n, bad;

	if ((fd = open_dev(path)) < 0)
		return (nfail);
	if (bo_new(fd, &cs, 4096) != 0 || bo_new(fd, &res, 4096) != 0)
		return (nfail);
	ok("command buffer at iova %#jx, result at %#jx (physical: the VRAM "
	    "carveout)", (uintmax_t)cs.iova, (uintmax_t)res.iova);
	memset(res.map, 0, 4096);
	/* CP_MEM_WRITE: address low, high, then the words */
	p = cs.map;
	n = 0;
	p[n++] = pkt7(CP_MEM_WRITE, 2 + 16);
	p[n++] = (uint32_t)res.iova;
	p[n++] = (uint32_t)(res.iova >> 32);
	for (i = 0; i < 16; i++)
		p[n++] = 0xc0de0000u | (uint32_t)i;
	p[n++] = pkt7(CP_NOP, 0);

	memset(bos, 0, sizeof(bos));
	bos[0].handle = cs.handle;
	bos[0].flags = MSM_SUBMIT_BO_READ;
	bos[0].presumed = cs.iova;
	bos[1].handle = res.handle;
	bos[1].flags = MSM_SUBMIT_BO_WRITE;
	bos[1].presumed = res.iova;
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = MSM_SUBMIT_CMD_BUF;
	cmd.submit_idx = 0;
	cmd.submit_offset = 0;
	cmd.size = n * 4;
	memset(&req, 0, sizeof(req));
	req.flags = MSM_PIPE_3D0;
	req.nr_bos = 2;
	req.nr_cmds = 1;
	req.bos = (uintptr_t)bos;
	req.cmds = (uintptr_t)&cmd;
	req.queueid = 0;
	t0 = now();
	if (drmCommandWriteRead(fd, DRM_MSM_GEM_SUBMIT, &req,
	    sizeof(req)) != 0) {
		fail("GEM_SUBMIT: %s%s", strerror(errno), errno == EPERM ?
		    " (root, or hw.msm.unprivileged_submit=1)" : "");
		return (nfail);
	}
	ok("submitted, fence %u", req.fence);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	memset(&wait, 0, sizeof(wait));
	wait.fence = req.fence;
	wait.queueid = 0;
	wait.timeout.tv_sec = ts.tv_sec + 1;
	wait.timeout.tv_nsec = ts.tv_nsec;
	if (drmCommandWrite(fd, DRM_MSM_WAIT_FENCE, &wait, sizeof(wait)) != 0) {
		fail("WAIT_FENCE %u: %s (not signalled within 1 s)", req.fence,
		    strerror(errno));
		return (nfail);
	}
	ok("fence %u signalled after %.1f ms", req.fence, (now() - t0) * 1e3);
	for (i = 0, bad = 0; i < 16; i++)
		if (res.map[i] != (0xc0de0000u | (uint32_t)i))
			bad++;
	if (bad == 0)
		ok("the GPU wrote the pattern (0xc0de0000..0xc0de000f)");
	else
		fail("pattern: %d of 16 words wrong (first %#x)", bad,
		    res.map[0]);
	close(fd);
	return (nfail);
}

/* --- KMS --------------------------------------------------------------------- */

struct dumb {
	uint32_t	handle, pitch, fb;
	uint64_t	size;
	uint32_t	*map;
};

static int
dumb_new(int fd, struct dumb *d, uint32_t w, uint32_t h)
{
	struct drm_mode_create_dumb c;
	struct drm_mode_map_dumb m;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	memset(&c, 0, sizeof(c));
	c.width = w;
	c.height = h;
	c.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0) {
		fail("CREATE_DUMB %ux%u: %s", w, h, strerror(errno));
		return (-1);
	}
	d->handle = c.handle;
	d->pitch = c.pitch;
	d->size = c.size;
	memset(&m, 0, sizeof(m));
	m.handle = d->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) != 0) {
		fail("MAP_DUMB: %s", strerror(errno));
		return (-1);
	}
	d->map = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)m.offset);
	if (d->map == MAP_FAILED) {
		fail("mmap of a dumb buffer: %s", strerror(errno));
		return (-1);
	}
	handles[0] = d->handle;
	pitches[0] = d->pitch;
	if (drmModeAddFB2(fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches,
	    offsets, &d->fb, 0) != 0) {
		fail("ADDFB2 XRGB8888 %ux%u pitch %u: %s", w, h, d->pitch,
		    strerror(errno));
		return (-1);
	}
	return (0);
}

/*
 * Eight colour bars across, a white frame and corner marks (top left red,
 * top right green, bottom left blue: which way the panel is turned), a
 * white bar at row y (moving)
 */
static void
pattern(struct dumb *d, uint32_t w, uint32_t h, uint32_t bar)
{
	static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff,
	    0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
	uint32_t x, y, c, *row;

	for (y = 0; y < h; y++) {
		row = d->map + (size_t)y * (d->pitch / 4);
		for (x = 0; x < w; x++) {
			c = bars[x * 8 / w];
			if (x < 8 || y < 8 || x >= w - 8 || y >= h - 8)
				c = 0xffffff;
			else if (x < 120 && y < 120)
				c = 0xff0000;
			else if (x >= w - 120 && y < 120)
				c = 0x00ff00;
			else if (x < 120 && y >= h - 120)
				c = 0x0000ff;
			if (y >= bar && y < bar + 40)
				c = 0xffffff;
			row[x] = c;
		}
	}
}

static int flips_done;

static void
flip_handler(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
    void *data)
{
	flips_done++;
}

static int
cmd_kms(const char *path, int seconds)
{
	drmModeResPtr res;
	drmModeConnectorPtr conn = NULL;
	drmModeEncoderPtr enc;
	drmModeCrtcPtr crtc;
	drmModePlaneResPtr pres;
	drmModePlanePtr plane;
	drmModeModeInfo mode;
	drmEventContext ev;
	drmVBlank vbl;
	struct dumb buf[2];
	struct pollfd pfd;
	uint32_t crtc_id = 0, conn_id, w, h, bar = 0, seq0;
	double t0, t;
	int fd, i, j, cur, queued, nflips, late;

	if ((fd = open_dev(path)) < 0)
		return (nfail);
	if (drmSetMaster(fd) != 0)
		printf("drmSetMaster: %s (another master?)\n", strerror(errno));
	res = drmModeGetResources(fd);
	if (res == NULL) {
		fail("GETRESOURCES: %s (no KMS: hw.msm.kms=1?)",
		    strerror(errno));
		return (nfail);
	}
	ok("KMS: %d CRTC, %d encoder, %d connector, fb %ux%u..%ux%u",
	    res->count_crtcs, res->count_encoders, res->count_connectors,
	    res->min_width, res->min_height, res->max_width, res->max_height);
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn == NULL)
			continue;
		printf("connector %u: type %u (%s), %s, %d modes, %ux%u mm\n",
		    conn->connector_id, conn->connector_type,
		    drmModeGetConnectorTypeName(conn->connector_type),
		    conn->connection == DRM_MODE_CONNECTED ? "connected" :
		    "not connected", conn->count_modes, conn->mmWidth,
		    conn->mmHeight);
		for (j = 0; j < conn->count_modes; j++)
			printf("  mode %s: %u kHz, %u %u %u %u, %u %u %u %u, "
			    "%u Hz\n", conn->modes[j].name,
			    conn->modes[j].clock, conn->modes[j].hdisplay,
			    conn->modes[j].hsync_start, conn->modes[j].hsync_end,
			    conn->modes[j].htotal, conn->modes[j].vdisplay,
			    conn->modes[j].vsync_start, conn->modes[j].vsync_end,
			    conn->modes[j].vtotal, conn->modes[j].vrefresh);
		if (conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0)
			break;
		drmModeFreeConnector(conn);
		conn = NULL;
	}
	if (conn == NULL) {
		fail("no connected connector with a mode");
		return (nfail);
	}
	ok("connector %s-1 connected, %s", drmModeGetConnectorTypeName(
	    conn->connector_type), conn->modes[0].name);
	mode = conn->modes[0];
	conn_id = conn->connector_id;
	w = mode.hdisplay;
	h = mode.vdisplay;
	if (conn->encoder_id != 0 &&
	    (enc = drmModeGetEncoder(fd, conn->encoder_id)) != NULL) {
		crtc_id = enc->crtc_id;
		drmModeFreeEncoder(enc);
	}
	if (crtc_id == 0 && res->count_crtcs > 0)
		crtc_id = res->crtcs[0];
	if ((crtc = drmModeGetCrtc(fd, crtc_id)) != NULL) {
		printf("crtc %u: fb %u, mode %s\n", crtc->crtc_id,
		    crtc->buffer_id, crtc->mode_valid ? crtc->mode.name :
		    "(none)");
		drmModeFreeCrtc(crtc);
	}
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if ((pres = drmModeGetPlaneResources(fd)) != NULL) {
		for (i = 0; i < (int)pres->count_planes; i++) {
			plane = drmModeGetPlane(fd, pres->planes[i]);
			if (plane == NULL)
				continue;
			printf("plane %u: crtcs %#x, formats", plane->plane_id,
			    plane->possible_crtcs);
			for (j = 0; j < (int)plane->count_formats; j++)
				printf(" %.4s", (char *)&plane->formats[j]);
			printf("\n");
			drmModeFreePlane(plane);
		}
		drmModeFreePlaneResources(pres);
	}

	/* kms1: the pattern, then flips */
	if (dumb_new(fd, &buf[0], w, h) != 0 || dumb_new(fd, &buf[1], w, h) != 0)
		return (nfail);
	ok("two dumb buffers %ux%u, pitch %u, %ju bytes", w, h, buf[0].pitch,
	    (uintmax_t)buf[0].size);
	pattern(&buf[0], w, h, 0);
	if (drmModeSetCrtc(fd, crtc_id, buf[0].fb, 0, 0, &conn_id, 1,
	    &mode) != 0) {
		fail("SETCRTC: %s", strerror(errno));
		return (nfail);
	}
	ok("SETCRTC: the test pattern on the panel (bars, white frame; red "
	    "top left, green top right, blue bottom left)");
	sleep(5);

	memset(&ev, 0, sizeof(ev));
	ev.version = 2;
	ev.page_flip_handler = flip_handler;
	pfd.fd = fd;
	pfd.events = POLLIN;
	cur = 0;
	nflips = late = 0;
	t0 = now();
	while ((t = now()) - t0 < seconds) {
		cur ^= 1;
		bar = (bar + 8) % h;
		pattern(&buf[cur], w, h, bar);
		queued = flips_done;
		if (drmModePageFlip(fd, crtc_id, buf[cur].fb,
		    DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0) {
			fail("PAGE_FLIP: %s", strerror(errno));
			break;
		}
		nflips++;
		/* the event within 100 ms (6 frames) */
		while (flips_done == queued) {
			if (poll(&pfd, 1, 100) <= 0) {
				late++;
				break;
			}
			drmHandleEvent(fd, &ev);
		}
		if (late > 3) {
			fail("no flip event within 100 ms, %d times", late);
			break;
		}
	}
	t = now() - t0;
	if (flips_done > 0 && late <= 3)
		ok("%d page flips, %d events in %.1f s: %.1f flips/s", nflips,
		    flips_done, t, flips_done / t);
	else
		fail("%d page flips, %d events in %.1f s", nflips, flips_done,
		    t);

	/* kms2: vblanks */
	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.sequence = 0;
	if (drmWaitVBlank(fd, &vbl) != 0) {
		fail("WAIT_VBLANK: %s", strerror(errno));
	} else {
		seq0 = vbl.reply.sequence;
		t0 = now();
		for (i = 0; i < 120; i++) {
			memset(&vbl, 0, sizeof(vbl));
			vbl.request.type = DRM_VBLANK_RELATIVE;
			vbl.request.sequence = 1;
			if (drmWaitVBlank(fd, &vbl) != 0)
				break;
		}
		t = now() - t0;
		if (i == 120 && vbl.reply.sequence - seq0 >= 120)
			ok("120 vblanks waited for in %.2f s: %.1f Hz "
			    "(sequence %u -> %u)", t, 120 / t, seq0,
			    vbl.reply.sequence);
		else
			fail("vblank wait %d: %s (sequence %u -> %u)", i,
			    strerror(errno), seq0, vbl.reply.sequence);
		if (i == 120 && (120 / t < 50 || 120 / t > 70))
			fail("vblank rate %.1f Hz, not about 60", 120 / t);
	}

	/* the buffers away: the console's framebuffer again */
	drmModeRmFB(fd, buf[0].fb);
	drmModeRmFB(fd, buf[1].fb);
	ok("framebuffers removed: the console shows again");
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	close(fd);
	return (nfail);
}

int
main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "info";
	const char *dev = argc > 2 ? argv[2] : "/dev/dri/renderD128";

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (strcmp(cmd, "info") == 0)
		return (cmd_info(dev));
	if (strcmp(cmd, "param") == 0)
		return (cmd_param(dev));
	if (strcmp(cmd, "submit") == 0)
		return (cmd_submit(dev));
	if (strcmp(cmd, "kms") == 0)
		return (cmd_kms("/dev/dri/card0", argc > 2 ? atoi(argv[2]) :
		    10));
	fprintf(stderr, "usage: pro1x-gpu info|param|submit [DEV] | kms "
	    "[SECONDS]\n");
	return (2);
}
