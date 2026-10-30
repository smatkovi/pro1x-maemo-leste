/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-fmaudio's register accesses, log and clock (fmaudio.h). On the
 * phone a window is a mapping of /dev/qcom_lpass (device memory, 32-bit
 * accesses only); in the host tests it is a simulation.
 */

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "fmaudio.h"

int fma_verbose;

static void
vlog(const char *fmt, va_list ap)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("%5ld.%03ld ", (long)ts.tv_sec % 100000,
	    (long)ts.tv_nsec / 1000000);
	vprintf(fmt, ap);
	putchar('\n');
	fflush(stdout);
}

void
fma_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

void
fma_dbg(const char *fmt, ...)
{
	va_list ap;

	if (!fma_verbose)
		return;
	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

uint64_t
fma_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

/* 0: a short pause in a polling loop (50 us) */
void
fma_sleep_ms(unsigned ms)
{
	usleep(ms ? ms * 1000 : 50);
}

uint32_t
mmio_rd(struct mmio *m, uint32_t off)
{
	uint32_t v;

	if (m->rd != NULL)
		v = m->rd(m, off);
	else
		v = m->base[off / 4];
	if (m->trace)
		fma_log("mmio: %s+%#x -> %#x", m->name, off, v);
	return (v);
}

void
mmio_wr(struct mmio *m, uint32_t off, uint32_t val)
{
	if (m->trace)
		fma_log("mmio: %s+%#x <- %#x", m->name, off, val);
	if (m->wr != NULL)
		m->wr(m, off, val);
	else
		m->base[off / 4] = val;
}
