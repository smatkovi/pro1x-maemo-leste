/*
 * N9 touch input for Harmattan's Qt on a current X server. Loaded into
 * every Harmattan process through /etc/ld.so.preload (38-hardware-adaptation.sh).
 *
 * Harmattan's Qt 4.7 makes QTouchEvents - which MeeGo Touch needs for
 * panning, MPanRecognizerTouch - only from the N9's X input: a "Max
 * Contacts" property on the device, and per contact the valuators "Abs MT
 * Position X/Y" in screen pixels and "Abs MT Tracking ID". With
 * xf86-input-libinput there is no such property, the positions are
 * 0..65535 of the unrotated panel, and no tracking ID exists, so Qt only
 * sees a mouse: taps work, lists do not scroll.
 *
 * This library answers "Max Contacts", gives touchscreen devices a tracking
 * ID valuator, and rewrites the pointer events X emulates from the first
 * touch: position = root (screen) coordinates, tracking ID 0 while the
 * finger is down. One contact only: the emulated pointer carries no more.
 *
 * libX11/libXi are not linked; the real functions come from the copies the
 * process has loaded, so non-X programs are unaffected.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>

#define EXPORT __attribute__((visibility("default")))
#define MAX_CONTACTS 10

static void *real(const char *lib, const char *name)
{
	void *h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
	if (!h)
		h = dlopen(lib, RTLD_NOW);
	return h ? dlsym(h, name) : NULL;
}
#define REAL(lib, name) \
	static __typeof__(name) *real_##name; \
	if (!real_##name) \
		real_##name = real(lib, #name)

static Atom atom(Display *dpy, const char *name)
{
	REAL("libX11.so.6", XInternAtom);
	return real_XInternAtom(dpy, name, False);
}

/* ---- the touchscreen's valuators ---------------------------------------- */

struct mt_axes {
	int x, y;	/* valuator numbers of Abs MT Position X/Y */
	int track;	/* number given to the tracking ID */
};

/* Find MT X/Y among a device's classes; track = first free number. */
static int find_axes(Display *dpy, XIAnyClassInfo **classes, int n, struct mt_axes *a)
{
	Atom ax = atom(dpy, "Abs MT Position X");
	Atom ay = atom(dpy, "Abs MT Position Y");
	Atom at = atom(dpy, "Abs MT Tracking ID");
	int top = -1;

	a->x = a->y = -1;
	for (int i = 0; i < n; i++) {
		if (classes[i]->type != XIValuatorClass)
			continue;
		XIValuatorClassInfo *v = (XIValuatorClassInfo *)classes[i];
		if (v->label == at)
			return 0;	/* a real N9-style device */
		if (v->label == ax)
			a->x = v->number;
		else if (v->label == ay)
			a->y = v->number;
		if (v->number > top)
			top = v->number;
	}
	a->track = top + 1;
	return a->x >= 0 && a->y >= 0 && a->track < 32;
}

/* Per source device, whether its events get rewritten. */
#define NSRC 16
static struct {
	Display *dpy;
	int id;
	int ok;
	struct mt_axes a;
} sources[NSRC];

static struct mt_axes *source_axes(Display *dpy, int id)
{
	int i, free_slot = -1;

	for (i = 0; i < NSRC; i++) {
		if (sources[i].dpy == dpy && sources[i].id == id)
			return sources[i].ok ? &sources[i].a : NULL;
		if (!sources[i].dpy && free_slot < 0)
			free_slot = i;
	}
	REAL("libXi.so.6", XIQueryDevice);
	REAL("libXi.so.6", XIFreeDeviceInfo);
	int n = 0, ok = 0;
	struct mt_axes a = { -1, -1, -1 };
	XIDeviceInfo *info = real_XIQueryDevice(dpy, id, &n);
	if (info && n == 1)
		ok = find_axes(dpy, info->classes, info->num_classes, &a);
	if (info)
		real_XIFreeDeviceInfo(info);
	if (free_slot < 0)
		free_slot = id % NSRC;
	sources[free_slot].dpy = dpy;
	sources[free_slot].id = id;
	sources[free_slot].ok = ok;
	sources[free_slot].a = a;
	return ok ? &sources[free_slot].a : NULL;
}

/* ---- "Max Contacts" ----------------------------------------------------- */

EXPORT Status XIGetProperty(Display *dpy, int deviceid, Atom property, long offset,
			    long length, Bool delete_property, Atom type, Atom *type_return,
			    int *format_return, unsigned long *num_items_return,
			    unsigned long *bytes_after_return, unsigned char **data)
{
	REAL("libXi.so.6", XIGetProperty);
	if (property == atom(dpy, "Max Contacts")) {
		*data = malloc(1);
		if (*data) {
			(*data)[0] = MAX_CONTACTS;
			*type_return = XA_INTEGER;
			*format_return = 8;
			*num_items_return = 1;
			*bytes_after_return = 0;
			return Success;
		}
	}
	return real_XIGetProperty(dpy, deviceid, property, offset, length, delete_property,
				  type, type_return, format_return, num_items_return,
				  bytes_after_return, data);
}

/* ---- device info: the tracking ID valuator ------------------------------ */

/* Devices whose class array this library replaced. libXi frees each
 * device's own array in XIFreeDeviceInfo, so it gets it back first. */
#define NDEV 32
static struct {
	XIDeviceInfo *info;	/* what XIQueryDevice returned */
	XIDeviceInfo *dev;	/* the entry within it */
	XIAnyClassInfo **orig;
	int orig_n;
} devs[NDEV];

EXPORT XIDeviceInfo *XIQueryDevice(Display *dpy, int deviceid, int *ndevices_return)
{
	REAL("libXi.so.6", XIQueryDevice);
	XIDeviceInfo *info = real_XIQueryDevice(dpy, deviceid, ndevices_return);
	if (!info)
		return info;
	int w = DisplayWidth(dpy, DefaultScreen(dpy));
	int h = DisplayHeight(dpy, DefaultScreen(dpy));

	for (int d = 0; d < *ndevices_return; d++) {
		XIDeviceInfo *dev = &info[d];
		struct mt_axes a;
		int slot;
		if (!find_axes(dpy, dev->classes, dev->num_classes, &a))
			continue;
		for (slot = 0; slot < NDEV && devs[slot].info; slot++)
			;
		if (slot == NDEV)
			continue;
		/* class pointers plus the tracking ID class, in one block */
		size_t n = dev->num_classes + 1;
		size_t off = (n * sizeof(XIAnyClassInfo *) + 7) & ~(size_t)7;
		XIAnyClassInfo **classes = calloc(1, off + sizeof(XIValuatorClassInfo));
		if (!classes)
			continue;
		XIValuatorClassInfo *t = (XIValuatorClassInfo *)((char *)classes + off);
		int src = dev->deviceid;
		for (int i = 0; i < dev->num_classes; i++) {
			classes[i] = dev->classes[i];
			if (classes[i]->type != XIValuatorClass)
				continue;
			XIValuatorClassInfo *v = (XIValuatorClassInfo *)classes[i];
			if (v->number == a.x || v->number == a.y) {
				v->min = 0;
				v->max = v->number == a.x ? w : h;
				src = v->sourceid;
			}
		}
		t->type = XIValuatorClass;
		t->sourceid = src;
		t->number = a.track;
		t->label = atom(dpy, "Abs MT Tracking ID");
		t->min = 0;
		t->max = MAX_CONTACTS - 1;
		t->mode = XIModeAbsolute;
		classes[n - 1] = (XIAnyClassInfo *)t;
		devs[slot].info = info;
		devs[slot].dev = dev;
		devs[slot].orig = dev->classes;
		devs[slot].orig_n = dev->num_classes;
		dev->classes = classes;
		dev->num_classes = n;
	}
	return info;
}

EXPORT void XIFreeDeviceInfo(XIDeviceInfo *info)
{
	REAL("libXi.so.6", XIFreeDeviceInfo);
	for (int i = 0; info && i < NDEV; i++) {
		if (devs[i].info != info)
			continue;
		free(devs[i].dev->classes);
		devs[i].dev->classes = devs[i].orig;
		devs[i].dev->num_classes = devs[i].orig_n;
		devs[i].info = NULL;
	}
	real_XIFreeDeviceInfo(info);
}

/* ---- events: screen position and tracking ID ---------------------------- */

static int xi_opcode(Display *dpy)
{
	static Display *last;
	static int op;
	int ev, err;
	if (dpy != last) {
		REAL("libX11.so.6", XQueryExtension);
		if (!real_XQueryExtension(dpy, "XInputExtension", &op, &ev, &err))
			op = -1;
		last = dpy;
	}
	return op;
}

/* Copy the event into one new block, as libXi allocates it (XFreeEventData
 * frees cookie->data as a whole), with the tracking ID valuator added. */
static XIDeviceEvent *rewrite(XIDeviceEvent *ev, const struct mt_axes *a, int down)
{
	int vlen = ev->valuators.mask_len;
	int need = (a->track >> 3) + 1;
	int nlen = vlen > need ? vlen : need;
	int nvals = 0;
	for (int i = 0; i < vlen * 8; i++)
		if (XIMaskIsSet(ev->valuators.mask, i))
			nvals++;
	size_t size = sizeof(*ev) + (nvals + 1) * sizeof(double) + ev->buttons.mask_len + nlen;
	char *p = calloc(1, size);
	if (!p)
		return NULL;
	XIDeviceEvent *n = (XIDeviceEvent *)p;
	*n = *ev;
	n->valuators.values = (double *)(p + sizeof(*ev));
	n->buttons.mask = (unsigned char *)(n->valuators.values + nvals + 1);
	n->valuators.mask = n->buttons.mask + ev->buttons.mask_len;
	n->valuators.mask_len = nlen;
	memcpy(n->buttons.mask, ev->buttons.mask, ev->buttons.mask_len);

	const double *src = ev->valuators.values;
	double *dst = n->valuators.values;
	for (int i = 0; i < nlen * 8; i++) {
		int set = i < vlen * 8 && XIMaskIsSet(ev->valuators.mask, i);
		if (i == a->track) {
			if (set)
				src++;
			if (down) {
				XISetMask(n->valuators.mask, i);
				*dst++ = 0;
			}
			continue;
		}
		if (!set)
			continue;
		XISetMask(n->valuators.mask, i);
		double v = *src++;
		if (i == a->x)
			v = ev->root_x;
		else if (i == a->y)
			v = ev->root_y;
		*dst++ = v;
	}
	return n;
}

EXPORT Bool XGetEventData(Display *dpy, XGenericEventCookie *cookie)
{
	REAL("libX11.so.6", XGetEventData);
	Bool ret = real_XGetEventData(dpy, cookie);
	if (!ret || cookie->extension != xi_opcode(dpy))
		return ret;
	if (cookie->evtype != XI_ButtonPress && cookie->evtype != XI_ButtonRelease &&
	    cookie->evtype != XI_Motion)
		return ret;

	XIDeviceEvent *ev = cookie->data;
	struct mt_axes *a = source_axes(dpy, ev->sourceid);
	if (!a)
		return ret;
	/* Fetched again after XPutBackEvent: already rewritten. */
	if (a->track < ev->valuators.mask_len * 8 && XIMaskIsSet(ev->valuators.mask, a->track))
		return ret;

	int down = ev->evtype == XI_ButtonPress ||
		   (ev->evtype == XI_Motion && ev->buttons.mask_len > 0 &&
		    XIMaskIsSet(ev->buttons.mask, 1));
	XIDeviceEvent *n = rewrite(ev, a, down);
	if (n) {
		free(cookie->data);
		cookie->data = n;
	}
	return ret;
}
