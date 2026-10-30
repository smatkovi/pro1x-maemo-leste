/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pro1x-volume-keys -- the Pro1X's volume keys in FreeBSD's Hildon session
 * (freebsd/audio/README.md, stage 4): the volume down key (the PMIC's,
 * qcom_pon) and the volume up key (gpio-keys) come into X as
 * XF86AudioLowerVolume and XF86AudioRaiseVolume; nothing in FreeBSD's
 * Hildon takes them, so this grabs them (and XF86AudioMute) on the root
 * window and steps the volume of PulseAudio's default sink by 5 %, up to
 * 100 %, with pactl. pro1x-volume-osd shows the change (it follows the
 * sink's volume). Started by hildon-session.
 *
 *   pro1x-volume-keys [-s STEP]
 */

#include <X11/Xlib.h>
#include <X11/XF86keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int grab_failed;

static int
on_error(Display *dpy, XErrorEvent *e)
{
	(void)dpy;
	if (e->error_code == BadAccess)
		grab_failed = 1;
	return (0);
}

/* the default sink's volume in %, from pactl get-sink-volume; -1: none */
static int
volume(void)
{
	char line[512], *p;
	FILE *f;
	int v = -1;

	if ((f = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null",
	    "r")) == NULL)
		return (-1);
	while (fgets(line, sizeof(line), f) != NULL)
		if (v < 0 && (p = strchr(line, '%')) != NULL) {
			while (p > line && p[-1] >= '0' && p[-1] <= '9')
				p--;
			v = atoi(p);
		}
	pclose(f);
	return (v);
}

int
main(int argc, char **argv)
{
	static const KeySym keys[] = { XF86XK_AudioRaiseVolume,
	    XF86XK_AudioLowerVolume, XF86XK_AudioMute };
	KeyCode code[3];
	Display *dpy;
	Window root;
	XEvent ev;
	char cmd[128];
	int c, i, step = 5, v;

	while ((c = getopt(argc, argv, "s:")) != -1)
		if (c == 's')
			step = atoi(optarg);
	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		fprintf(stderr, "pro1x-volume-keys: no display\n");
		return (1);
	}
	root = DefaultRootWindow(dpy);
	XSetErrorHandler(on_error);
	for (i = 0; i < 3; i++) {
		code[i] = XKeysymToKeycode(dpy, keys[i]);
		if (code[i] != 0)
			XGrabKey(dpy, code[i], AnyModifier, root, True,
			    GrabModeAsync, GrabModeAsync);
	}
	XSync(dpy, False);
	if (grab_failed)
		fprintf(stderr, "pro1x-volume-keys: a volume key is taken by "
		    "another client\n");
	for (;;) {
		XNextEvent(dpy, &ev);
		if (ev.type != KeyPress)
			continue;
		if (ev.xkey.keycode == code[0]) {
			v = volume();
			if (v >= 0 && v + step > 100)
				snprintf(cmd, sizeof(cmd), "pactl set-sink-volume "
				    "@DEFAULT_SINK@ 100%%");
			else
				snprintf(cmd, sizeof(cmd), "pactl set-sink-volume "
				    "@DEFAULT_SINK@ +%d%%", step);
			(void)system("pactl set-sink-mute @DEFAULT_SINK@ 0");
		} else if (ev.xkey.keycode == code[1])
			snprintf(cmd, sizeof(cmd), "pactl set-sink-volume "
			    "@DEFAULT_SINK@ -%d%%", step);
		else if (ev.xkey.keycode == code[2])
			snprintf(cmd, sizeof(cmd), "pactl set-sink-mute "
			    "@DEFAULT_SINK@ toggle");
		else
			continue;
		(void)system(cmd);
	}
}
