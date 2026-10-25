/*
 * Edge swipes of Harmattan's mcompositor on a touchscreen that is not the
 * N9's. Preloaded into mcompositor (40-harmattan-session.sh).
 *
 * The swipe extension (libnokiaswipeextension.so) reads the finger from the
 * XI2 events on its edge window: the valuators "Abs MT Position X/Y" in
 * device units, which on the N9 were screen pixels, and "Abs MT Tracking ID"
 * = 0 for a finger on the glass. xf86-input-libinput reports the position
 * in 0..65535 of the unrotated panel and has no tracking ID valuator, so the
 * extension never sees a finger. Taps it passes on to the application go
 * back through XTest as device events with the same N9 valuators.
 *
 * Here the finger comes from the events' root coordinates (rotated, scaled
 * screen pixels), and the replayed taps are core XTest events at those
 * screen coordinates.
 */
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#define EXPORT __attribute__((visibility("default")))

/* MSwipeGestureExtension::extractStateOfPrimaryTouchpoint(XIDeviceEvent const*, int*, int*, bool*) */
EXPORT void
_ZN22MSwipeGestureExtension31extractStateOfPrimaryTouchpointEPK13XIDeviceEventPiS3_Pb(
	void *self, const XIDeviceEvent *ev, int *x, int *y, bool *pressed)
{
	(void)self;
	*x = (int)(ev->root_x + 0.5);
	*y = (int)(ev->root_y + 0.5);
	switch (ev->evtype) {
	case XI_ButtonPress:
		*pressed = true;
		break;
	case XI_Motion:
		/* touch emulates button 1 */
		*pressed = ev->buttons.mask_len > 0 && XIMaskIsSet(ev->buttons.mask, 1);
		break;
	default:
		*pressed = false;
		break;
	}
}

EXPORT int
XTestFakeDeviceMotionEvent(Display *dpy, XDevice *dev, Bool is_relative,
			   int first_axis, int *axes, int n_axes, unsigned long delay)
{
	(void)dev;
	(void)is_relative;
	if (first_axis != 0 || n_axes < 2)
		return 1;
	return XTestFakeMotionEvent(dpy, -1, axes[0], axes[1], delay);
}

EXPORT int
XTestFakeDeviceButtonEvent(Display *dpy, XDevice *dev, unsigned int button,
			   Bool is_press, int *axes, int n_axes, unsigned long delay)
{
	(void)dev;
	if (n_axes >= 2)
		XTestFakeMotionEvent(dpy, -1, axes[0], axes[1], CurrentTime);
	return XTestFakeButtonEvent(dpy, button, is_press, delay);
}
