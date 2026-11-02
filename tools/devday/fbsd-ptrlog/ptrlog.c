/*
 * ptrlog: where X has the pointer, for the touchscreen's orientation on
 * the Pro1X (tools/devday/fbsd-x.sh runs it). The screen's size once,
 * then a line whenever the pointer moves or a button changes: seconds,
 * root x, root y, the button mask. Polls every 50 ms; ends with X.
 *
 *   cc -O2 -o ptrlog ptrlog.c -lX11   (in the FreeBSD VM)
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int
main(void)
{
	Display *d = XOpenDisplay(NULL);
	Window root, rr, cr;
	int rx, ry, wx, wy, lx = -1, ly = -1;
	unsigned int m, lm = ~0u;
	struct timespec t0, t;

	if (d == NULL) {
		fprintf(stderr, "ptrlog: no display\n");
		return (1);
	}
	root = DefaultRootWindow(d);
	printf("screen %dx%d\n", DisplayWidth(d, 0), DisplayHeight(d, 0));
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (!XQueryPointer(d, root, &rr, &cr, &rx, &ry, &wx, &wy, &m))
			break;
		if (rx != lx || ry != ly || m != lm) {
			clock_gettime(CLOCK_MONOTONIC, &t);
			printf("%.2f %d %d %#x\n", (t.tv_sec - t0.tv_sec) +
			    (t.tv_nsec - t0.tv_nsec) / 1e9, rx, ry, m);
			fflush(stdout);
			lx = rx;
			ly = ry;
			lm = m;
		}
		usleep(50000);
	}
	return (0);
}
