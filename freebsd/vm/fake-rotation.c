/*
 * fake-rotation.c: RandR rotation for a screen that cannot rotate, for
 * trying Hildon's portrait mode in the test VM (try-meego-portrait.sh).
 *
 * hildon-desktop goes to portrait by rotating the screen's CRTC 90 degrees
 * (XRRSetCrtcConfig with RR_Rotate_90 and a screen of the other size).
 * The VM's X server (Xorg's dummy driver) can change its screen's size and
 * mode but cannot rotate. Preloaded (LD_PRELOAD), this makes the CRTC
 * report all four rotations and the one last asked for, and sets a mode of
 * the other shape instead of a rotated one: 90 or 270 degrees of the
 * landscape mode become the portrait mode (1080x2160) unrotated, 0 or 180
 * the landscape mode again. The screen then has the size and shape it has
 * on the phone after the rotation, which is what the windows see; the
 * picture is not turned (the screenshots are upright).
 *
 *   cc -shared -fPIC -o fake-rotation.so fake-rotation.c -I/usr/local/include \
 *       -L/usr/local/lib -lXrandr -lX11
 */
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

static Rotation rotation = RR_Rotate_0;

XRRCrtcInfo *
XRRGetCrtcInfo (Display *dpy, XRRScreenResources *res, RRCrtc crtc)
{
  static XRRCrtcInfo *(*real) (Display *, XRRScreenResources *, RRCrtc);
  XRRCrtcInfo *info;

  if (!real)
    real = dlsym (RTLD_NEXT, "XRRGetCrtcInfo");
  info = real (dpy, res, crtc);
  if (info)
    {
      info->rotations |= RR_Rotate_0 | RR_Rotate_90 | RR_Rotate_180
                         | RR_Rotate_270;
      if (info->mode != None)
        info->rotation = rotation;
    }
  return info;
}

/* @mode or, if it has not that shape (portrait: higher than wide), the
 * screen's mode of its size turned */
static RRMode
mode_shaped (XRRScreenResources *res, RRMode mode, int portrait)
{
  unsigned int w = 0, h = 0;
  int i;

  for (i = 0; i < res->nmode; i++)
    if (res->modes[i].id == mode)
      w = res->modes[i].width, h = res->modes[i].height;
  if (portrait ? h >= w : w >= h)
    return mode;
  for (i = 0; i < res->nmode; i++)
    if (res->modes[i].width == h && res->modes[i].height == w)
      return res->modes[i].id;
  return mode;
}

Status
XRRSetCrtcConfig (Display *dpy, XRRScreenResources *res, RRCrtc crtc,
                  Time timestamp, int x, int y, RRMode mode,
                  Rotation rot, RROutput *outputs, int noutputs)
{
  static Status (*real) (Display *, XRRScreenResources *, RRCrtc, Time,
                         int, int, RRMode, Rotation, RROutput *, int);

  if (!real)
    real = dlsym (RTLD_NEXT, "XRRSetCrtcConfig");
  if (mode != None)
    {
      mode = mode_shaped (res, mode,
                          (rot & (RR_Rotate_90 | RR_Rotate_270)) != 0);
      rotation = rot & 0xf;
      rot = (rot & ~0xf) | RR_Rotate_0;
    }
  return real (dpy, res, crtc, timestamp, x, y, mode, rot, outputs, noutputs);
}
