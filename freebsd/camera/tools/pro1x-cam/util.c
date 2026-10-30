/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-cam's messages and sleeps (also linked into the camera app).
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "cam.h"

int verbose;

void
msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

void
vmsg(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	printf("  ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

void
msleep(u_int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}
