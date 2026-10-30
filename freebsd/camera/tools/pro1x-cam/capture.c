/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A stream through /dev/camss0 (stage 2): the kernel's path configured
 * for the sensor's mode (CAMSS_IOC_STREAMON with its size, link frequency
 * and pixel rate), the buffers mapped, then the sensor started as Linux's
 * camss does it last (its registers, MODE_SELECT = 1), frames dequeued and
 * queued again; on the way back the sensor stopped first.
 */

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <err.h>
#include <errno.h>
#include <string.h>

#include "cam.h"
#include "sensor_tables.h"

#define	NBUFS	4

int
capture(struct cam *c, int s, const struct sensor_mode *m, u_int skip,
    u_int nframes, int (*cb)(struct cam *, int, struct frame *, void *),
    void *arg)
{
	const struct sensor_ops *o = sensor_ops(c, s);
	struct camss_stream st;
	struct camss_buf b;
	struct frame f;
	uint8_t *map;
	u_int got = 0, timeouts = 0, errors = 0;
	int error = 0, r;

	memset(&st, 0, sizeof(st));
	st.sensor = s;
	st.width = m->width;
	st.height = m->height;
	st.bpp = 10;
	st.nbufs = NBUFS;
	st.link_freq = o->link_freqs[m->link_freq_idx];
	st.pixel_rate = m->pixel_rate;
	if (ioctl(c->fd, CAMSS_IOC_STREAMON, &st) < 0) {
		warn("%s: STREAMON %s", cam_name(c, s), m->name);
		return (-1);
	}
	msg("%s: stream %s, %u buffers of %u bytes, stride %u; CPHY %u, timer "
	    "%u, CSID %u, TFE %u Hz, settle %u", cam_name(c, s), m->name,
	    st.nbufs, st.bufsize, st.stride, st.clk_cphy, st.clk_timer,
	    st.clk_csid, st.clk_tfe, st.settle_cnt);
	map = mmap(NULL, (size_t)st.bufsize * st.nbufs, PROT_READ, MAP_SHARED,
	    c->fd, 0);
	if (map == MAP_FAILED) {
		warn("mmap");
		ioctl(c->fd, CAMSS_IOC_STREAMOFF);
		return (-1);
	}
	if (o->start(c, s, m) < 0) {
		warnx("%s: the sensor did not start", cam_name(c, s));
		error = -1;
		goto out;
	}
	while (got < skip + nframes) {
		memset(&b, 0, sizeof(b));
		b.timeout_ms = 1000;
		if (ioctl(c->fd, CAMSS_IOC_DQBUF, &b) < 0) {
			if (errno != EAGAIN) {
				warn("DQBUF");
				error = -1;
				break;
			}
			msg("%s: no frame within 1 s (TFE status %#x, bus %#x, "
			    "overflow %#x, violation %#x)", cam_name(c, s),
			    b.tfe_status, b.bus_status, b.overflow, b.violation);
			if (++timeouts == 3) {
				error = -1;
				break;
			}
			continue;
		}
		vmsg("frame %u in buffer %u%s, bus status %#x", b.sequence,
		    b.index, b.flags & CAMSS_BUF_ERROR ? " (error)" : "",
		    b.bus_status);
		if (b.flags & CAMSS_BUF_ERROR)
			errors++;
		if (got++ >= skip && cb != NULL) {
			f.width = st.width;
			f.height = st.height;
			f.stride = st.stride;
			f.data = map + (size_t)b.index * st.bufsize;
			f.sequence = b.sequence;
			f.flags = b.flags;
			r = cb(c, s, &f, arg);
			if (r != 0) {
				error = r < 0 ? -1 : 0;
				ioctl(c->fd, CAMSS_IOC_QBUF, &b);
				break;
			}
		}
		if (ioctl(c->fd, CAMSS_IOC_QBUF, &b) < 0) {
			warn("QBUF");
			error = -1;
			break;
		}
	}
	o->stop(c, s);
out:
	msg("%s: %u frames, %u with bus errors, %u timeouts", cam_name(c, s),
	    got, errors, timeouts);
	munmap(map, (size_t)st.bufsize * st.nbufs);
	ioctl(c->fd, CAMSS_IOC_STREAMOFF);
	return (error);
}
