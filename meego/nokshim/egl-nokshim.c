/*
 * libEGL.so for Harmattan on Mesa: the Nokia EGL extensions of the N9's
 * PowerVR driver, on top of Mesa's libEGL.so.1.
 *
 * Harmattan binaries link the unversioned name libEGL.so (the SGX driver's
 * soname). This library takes that name, forwards everything to Mesa and
 * adds what Qt's "meego" graphics system and MeeGo Touch rely on:
 *
 *  EGL_NOK_image_shared   images with a global handle, used by the theme
 *                         daemon to hand its graphics to every application.
 *                         Implemented with dma-buf export/import; the file
 *                         descriptors are kept by nok-image-broker.
 *  EGL_KHR_lock_surface2  CPU access to pixmap surfaces, used for the
 *                         "live pixmaps" (status bar) of the meego graphics
 *                         system. Done with XGetImage/XPutImage on the pixmap.
 *  EGL_NOK_surface_scaling  accepted and ignored.
 *  eglCreateImageKHR & co   exported as symbols, as the SGX library did.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <drm_fourcc.h>

#include "nokshim.h"

#define EXPORT __attribute__((visibility("default")))

#define EGL_SHARED_IMAGE_NOK	0x30DA
#define EGL_FIXED_WIDTH_NOK	0x30DB
#define EGL_FIXED_HEIGHT_NOK	0x30DC
#ifndef GL_TEXTURE_WIDTH
#define GL_TEXTURE_WIDTH	0x1000
#define GL_TEXTURE_HEIGHT	0x1001
#endif

typedef void *EGLNativeSharedImageTypeNOK;

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const char *(*real_QueryString)(EGLDisplay, EGLint);
static __eglMustCastToProperFunctionPointerType (*real_GetProcAddress)(const char *);
static EGLSurface (*real_CreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLSurface (*real_CreatePixmapSurface)(EGLDisplay, EGLConfig, EGLNativePixmapType, const EGLint *);
static EGLBoolean (*real_DestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*real_QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static PFNEGLCREATEIMAGEKHRPROC real_CreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC real_DestroyImageKHR;
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC real_ExportQuery;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC real_Export;
static void (*gl_GetIntegerv)(GLenum, GLint *);
static void (*gl_BindTexture)(GLenum, GLuint);
static void (*gl_GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);

static void init(void)
{
	/* Local handles only: made global, Mesa's EGL would come before this
	 * shim for everything bound later (in processes such as Python, where
	 * the shim itself is not global, Qt's graphics system plugin). */
	void *egl = dlopen("libEGL.so.1", RTLD_NOW);
	void *gles = dlopen("libGLESv2.so.2", RTLD_NOW);
	real_QueryString = dlsym(egl, "eglQueryString");
	real_GetProcAddress = dlsym(egl, "eglGetProcAddress");
	real_CreateWindowSurface = dlsym(egl, "eglCreateWindowSurface");
	real_CreatePixmapSurface = dlsym(egl, "eglCreatePixmapSurface");
	real_DestroySurface = dlsym(egl, "eglDestroySurface");
	real_QuerySurface = dlsym(egl, "eglQuerySurface");
	real_CreateImageKHR = (void *)real_GetProcAddress("eglCreateImageKHR");
	real_DestroyImageKHR = (void *)real_GetProcAddress("eglDestroyImageKHR");
	real_ExportQuery = (void *)real_GetProcAddress("eglExportDMABUFImageQueryMESA");
	real_Export = (void *)real_GetProcAddress("eglExportDMABUFImageMESA");
	gl_GetIntegerv = dlsym(gles, "glGetIntegerv");
	gl_BindTexture = dlsym(gles, "glBindTexture");
	gl_GetTexLevelParameteriv = (void *)real_GetProcAddress("glGetTexLevelParameteriv");
}

#define INIT() pthread_once(&once, init)

/* ---- sizes of the images we created or imported ---------------------- */

struct size_entry {
	EGLImageKHR image;
	EGLint w, h;
};
static struct size_entry *sizes;
static int nsizes, capsizes;

static void size_set(EGLImageKHR image, EGLint w, EGLint h)
{
	pthread_mutex_lock(&lock);
	if (nsizes == capsizes) {
		int c = capsizes ? capsizes * 2 : 256;
		struct size_entry *s = realloc(sizes, c * sizeof(*s));
		if (!s) {
			pthread_mutex_unlock(&lock);
			return;
		}
		sizes = s;
		capsizes = c;
	}
	sizes[nsizes++] = (struct size_entry){ image, w, h };
	pthread_mutex_unlock(&lock);
}

static int size_get(EGLImageKHR image, EGLint *w, EGLint *h)
{
	int found = 0;
	pthread_mutex_lock(&lock);
	for (int i = nsizes - 1; i >= 0; i--) {
		if (sizes[i].image == image) {
			*w = sizes[i].w;
			*h = sizes[i].h;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

static void size_drop(EGLImageKHR image)
{
	pthread_mutex_lock(&lock);
	for (int i = 0; i < nsizes; i++) {
		if (sizes[i].image == image) {
			sizes[i] = sizes[--nsizes];
			break;
		}
	}
	pthread_mutex_unlock(&lock);
}

/* ---- broker connection ------------------------------------------------ */

static int broker = -1;

static int broker_connect_once(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int n = nok_socket_name(sa.sun_path, sizeof(sa.sun_path), getuid());
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, offsetof(struct sockaddr_un, sun_path) + n) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void spawn_broker(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		if (fork() == 0) {
			setsid();
			for (int fd = 0; fd < 1024; fd++)
				close(fd);
			execl(NOK_BROKER_PATH, "nok-image-broker", (char *)NULL);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* Called with lock held. */
static int broker_fd(void)
{
	if (broker >= 0)
		return broker;
	broker = broker_connect_once();
	if (broker >= 0)
		return broker;
	spawn_broker();
	for (int i = 0; i < 100 && broker < 0; i++) {
		struct timespec ts = { 0, 20 * 1000 * 1000 };
		nanosleep(&ts, NULL);
		broker = broker_connect_once();
	}
	return broker;
}

static int xfer(struct nok_msg *m, const int *fds, int nfds, int want_reply, int *rfds, int *nrfds)
{
	char cbuf[CMSG_SPACE(sizeof(int) * NOK_MAX_PLANES)];
	int fd = broker_fd();
	if (fd < 0)
		return -1;

	struct iovec iov = { m, sizeof(*m) };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
	if (nfds) {
		mh.msg_control = cbuf;
		mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
		memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
	}
	if (sendmsg(fd, &mh, MSG_NOSIGNAL) != sizeof(*m))
		goto broken;
	if (!want_reply)
		return 0;

	struct msghdr rh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
	};
	if (recvmsg(fd, &rh, MSG_CMSG_CLOEXEC) != sizeof(*m))
		goto broken;
	if (nrfds)
		*nrfds = 0;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&rh); c; c = CMSG_NXTHDR(&rh, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && rfds) {
			int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			memcpy(rfds, CMSG_DATA(c), k * sizeof(int));
			*nrfds = k;
		}
	}
	return 0;

broken:
	close(broker);
	broker = -1;
	return -1;
}

/* ---- EGL_NOK_image_shared --------------------------------------------- */

static EGLNativeSharedImageTypeNOK nok_CreateSharedImage(EGLDisplay dpy, EGLImageKHR image, EGLint *props)
{
	(void)props;
	INIT();
	struct nok_msg m = { .op = NOK_REGISTER };
	int fourcc, planes, fds[NOK_MAX_PLANES];
	EGLint strides[NOK_MAX_PLANES], offsets[NOK_MAX_PLANES];
	EGLuint64KHR modifier = DRM_FORMAT_MOD_INVALID;
	EGLint w, h;

	if (!real_ExportQuery || !real_Export || !size_get(image, &w, &h))
		return 0;
	if (!real_ExportQuery(dpy, image, &fourcc, &planes, &modifier) ||
	    planes < 1 || planes > NOK_MAX_PLANES)
		return 0;
	if (!real_Export(dpy, image, fds, strides, offsets))
		return 0;

	m.image.width = w;
	m.image.height = h;
	m.image.fourcc = fourcc;
	m.image.planes = planes;
	m.image.modifier = modifier;
	for (int i = 0; i < planes; i++) {
		m.image.strides[i] = strides[i];
		m.image.offsets[i] = offsets[i];
	}

	pthread_mutex_lock(&lock);
	int r = xfer(&m, fds, planes, 1, NULL, NULL);
	pthread_mutex_unlock(&lock);
	for (int i = 0; i < planes; i++)
		close(fds[i]);
	if (r < 0)
		return 0;
	return (EGLNativeSharedImageTypeNOK)(uintptr_t)m.handle;
}

static EGLBoolean nok_DestroySharedImage(EGLDisplay dpy, EGLNativeSharedImageTypeNOK handle)
{
	(void)dpy;
	struct nok_msg m = { .op = NOK_DESTROY, .handle = (uint32_t)(uintptr_t)handle };
	pthread_mutex_lock(&lock);
	int r = xfer(&m, NULL, 0, 0, NULL, NULL);
	pthread_mutex_unlock(&lock);
	return r == 0;
}

static EGLBoolean nok_QueryImage(EGLDisplay dpy, EGLImageKHR image, EGLint attr, EGLint *value)
{
	(void)dpy;
	EGLint w, h;
	if (!value || !size_get(image, &w, &h))
		return EGL_FALSE;
	if (attr == EGL_WIDTH)
		*value = w;
	else if (attr == EGL_HEIGHT)
		*value = h;
	else
		return EGL_FALSE;
	return EGL_TRUE;
}

static EGLImageKHR import_shared(EGLDisplay dpy, uint32_t handle)
{
	struct nok_msg m = { .op = NOK_LOOKUP, .handle = handle };
	int fds[NOK_MAX_PLANES], nfds = 0;

	pthread_mutex_lock(&lock);
	int r = xfer(&m, NULL, 0, 1, fds, &nfds);
	pthread_mutex_unlock(&lock);
	if (r < 0 || !m.handle || (uint32_t)nfds != m.image.planes)
		goto fail;

	static const EGLint plane_attr[NOK_MAX_PLANES][5] = {
		{ EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
		  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		  EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
		  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		  EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
	};
	EGLint attrs[8 + NOK_MAX_PLANES * 10 + 1];
	int n = 0;
	attrs[n++] = EGL_WIDTH;
	attrs[n++] = m.image.width;
	attrs[n++] = EGL_HEIGHT;
	attrs[n++] = m.image.height;
	attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	attrs[n++] = m.image.fourcc;
	for (uint32_t i = 0; i < m.image.planes; i++) {
		attrs[n++] = plane_attr[i][0];
		attrs[n++] = fds[i];
		attrs[n++] = plane_attr[i][1];
		attrs[n++] = m.image.offsets[i];
		attrs[n++] = plane_attr[i][2];
		attrs[n++] = m.image.strides[i];
		if (m.image.modifier != DRM_FORMAT_MOD_INVALID) {
			attrs[n++] = plane_attr[i][3];
			attrs[n++] = (EGLint)(m.image.modifier & 0xffffffff);
			attrs[n++] = plane_attr[i][4];
			attrs[n++] = (EGLint)(m.image.modifier >> 32);
		}
	}
	attrs[n++] = EGL_NONE;

	EGLImageKHR img = real_CreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	for (int i = 0; i < nfds; i++)
		close(fds[i]);
	if (img != EGL_NO_IMAGE_KHR)
		size_set(img, m.image.width, m.image.height);
	return img;

fail:
	for (int i = 0; i < nfds; i++)
		close(fds[i]);
	return EGL_NO_IMAGE_KHR;
}

static EGLBoolean nok_SetSurfaceScaling(EGLDisplay dpy, EGLSurface s, EGLint x, EGLint y, EGLint w, EGLint h)
{
	(void)dpy; (void)s; (void)x; (void)y; (void)w; (void)h;
	return EGL_TRUE;
}

/* ---- EGL_KHR_lock_surface2 on pixmap surfaces --------------------------- */

struct pixsurf {
	EGLSurface surface;		/* Mesa's, or a placeholder if Mesa refused */
	int fake;
	Pixmap pixmap;
	XImage *locked;
	struct pixsurf *next;
};
static struct pixsurf *pixsurfs;
static Display *xdpy;

/* Called with lock held. */
static struct pixsurf *pixsurf_find(EGLSurface s)
{
	for (struct pixsurf *p = pixsurfs; p; p = p->next)
		if (p->surface == s)
			return p;
	return NULL;
}

static Display *own_display(void)
{
	if (!xdpy)
		xdpy = XOpenDisplay(NULL);
	return xdpy;
}

static EGLBoolean nok_LockSurface(EGLDisplay dpy, EGLSurface surface, const EGLint *attrs)
{
	(void)dpy; (void)attrs;
	EGLBoolean ok = EGL_FALSE;
	pthread_mutex_lock(&lock);
	struct pixsurf *p = pixsurf_find(surface);
	Display *d = own_display();
	if (p && d && !p->locked) {
		Window root;
		int x, y;
		unsigned w, h, bw, depth;
		if (XGetGeometry(d, p->pixmap, &root, &x, &y, &w, &h, &bw, &depth)) {
			p->locked = XGetImage(d, p->pixmap, 0, 0, w, h, AllPlanes, ZPixmap);
			ok = p->locked != NULL;
		}
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

static EGLBoolean nok_UnlockSurface(EGLDisplay dpy, EGLSurface surface)
{
	(void)dpy;
	EGLBoolean ok = EGL_FALSE;
	pthread_mutex_lock(&lock);
	struct pixsurf *p = pixsurf_find(surface);
	if (p && p->locked && xdpy) {
		GC gc = XCreateGC(xdpy, p->pixmap, 0, NULL);
		XPutImage(xdpy, p->pixmap, gc, p->locked, 0, 0, 0, 0,
			  p->locked->width, p->locked->height);
		XFreeGC(xdpy, gc);
		XSync(xdpy, False);
		XDestroyImage(p->locked);
		p->locked = NULL;
		ok = EGL_TRUE;
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

/* ---- overridden and added entry points --------------------------------- */

EXPORT EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
					 EGLNativePixmapType pixmap, const EGLint *attrs)
{
	INIT();
	EGLSurface s = real_CreatePixmapSurface(dpy, config, pixmap, attrs);
	struct pixsurf *p = calloc(1, sizeof(*p));
	if (!p)
		return s;
	if (s == EGL_NO_SURFACE) {
		/* Mesa has no pixmap config for this visual; the surface is
		 * still good for locking, which is all live pixmaps need. */
		p->fake = 1;
		s = (EGLSurface)p;
	}
	p->surface = s;
	p->pixmap = (Pixmap)pixmap;
	pthread_mutex_lock(&lock);
	p->next = pixsurfs;
	pixsurfs = p;
	pthread_mutex_unlock(&lock);
	return s;
}

EXPORT EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
	INIT();
	int fake = 0;
	pthread_mutex_lock(&lock);
	for (struct pixsurf **pp = &pixsurfs; *pp; pp = &(*pp)->next) {
		if ((*pp)->surface == surface) {
			struct pixsurf *p = *pp;
			*pp = p->next;
			fake = p->fake;
			if (p->locked)
				XDestroyImage(p->locked);
			free(p);
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return fake ? EGL_TRUE : real_DestroySurface(dpy, surface);
}

EXPORT EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attr, EGLint *value)
{
	INIT();
	int handled = 0, fake = 0;
	EGLBoolean ok = EGL_FALSE;
	pthread_mutex_lock(&lock);
	struct pixsurf *p = pixsurf_find(surface);
	if (p) {
		fake = p->fake;
		XImage *img = p->locked;
		handled = 1;
		ok = EGL_TRUE;
		switch (attr) {
		case EGL_BITMAP_POINTER_KHR:
			ok = img != NULL;
			if (img)
				*value = (EGLint)(uintptr_t)img->data;
			break;
		case EGL_BITMAP_PITCH_KHR:
			ok = img != NULL;
			if (img)
				*value = img->bytes_per_line;
			break;
		case EGL_BITMAP_ORIGIN_KHR:
			*value = EGL_UPPER_LEFT_KHR;
			break;
		case EGL_BITMAP_PIXEL_SIZE_KHR:
			ok = img != NULL;
			if (img)
				*value = img->bits_per_pixel;
			break;
		case EGL_WIDTH:
		case EGL_HEIGHT:
			if (img) {
				*value = attr == EGL_WIDTH ? img->width : img->height;
			} else if (fake && own_display()) {
				Window root;
				int x, y;
				unsigned w, h, bw, depth;
				ok = XGetGeometry(xdpy, p->pixmap, &root, &x, &y, &w, &h, &bw, &depth);
				*value = attr == EGL_WIDTH ? (EGLint)w : (EGLint)h;
			} else {
				handled = 0;
			}
			break;
		default:
			handled = fake;
			ok = EGL_FALSE;
		}
	}
	pthread_mutex_unlock(&lock);
	if (handled)
		return ok;
	return real_QuerySurface(dpy, surface, attr, value);
}

EXPORT EGLBoolean eglLockSurfaceKHR(EGLDisplay dpy, EGLSurface surface, const EGLint *attrs)
{
	return nok_LockSurface(dpy, surface, attrs);
}

EXPORT EGLBoolean eglUnlockSurfaceKHR(EGLDisplay dpy, EGLSurface surface)
{
	return nok_UnlockSurface(dpy, surface);
}


EXPORT EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
				     EGLClientBuffer buffer, const EGLint *attrs)
{
	INIT();
	if (target == EGL_SHARED_IMAGE_NOK)
		return import_shared(dpy, (uint32_t)(uintptr_t)buffer);

	EGLImageKHR img = real_CreateImageKHR(dpy, ctx, target, buffer, attrs);
	if (img != EGL_NO_IMAGE_KHR && target == EGL_GL_TEXTURE_2D_KHR &&
	    gl_GetTexLevelParameteriv && gl_GetIntegerv && gl_BindTexture) {
		GLint level = 0, prev = 0, w = 0, h = 0;
		for (const EGLint *a = attrs; a && *a != EGL_NONE; a += 2)
			if (a[0] == EGL_GL_TEXTURE_LEVEL_KHR)
				level = a[1];
		gl_GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
		gl_BindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)buffer);
		gl_GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
		gl_GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
		gl_BindTexture(GL_TEXTURE_2D, prev);
		if (w > 0 && h > 0)
			size_set(img, w, h);
	}
	return img;
}

EXPORT EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image)
{
	INIT();
	size_drop(image);
	return real_DestroyImageKHR(dpy, image);
}

EXPORT EGLNativeSharedImageTypeNOK eglCreateSharedImageNOK(EGLDisplay dpy, EGLImageKHR image, EGLint *props)
{
	return nok_CreateSharedImage(dpy, image, props);
}

EXPORT EGLBoolean eglDestroySharedImageNOK(EGLDisplay dpy, EGLNativeSharedImageTypeNOK img)
{
	return nok_DestroySharedImage(dpy, img);
}

EXPORT EGLBoolean eglQueryImageNOK(EGLDisplay dpy, EGLImageKHR image, EGLint attr, EGLint *value)
{
	return nok_QueryImage(dpy, image, attr, value);
}

EXPORT EGLBoolean eglSetSurfaceScalingNOK(EGLDisplay dpy, EGLSurface s, EGLint x, EGLint y, EGLint w, EGLint h)
{
	return nok_SetSurfaceScaling(dpy, s, x, y, w, h);
}

EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
					 EGLNativeWindowType win, const EGLint *attrs)
{
	INIT();
	EGLint clean[64];
	int n = 0;
	for (const EGLint *a = attrs; a && *a != EGL_NONE && n < 62; a += 2) {
		if (a[0] == EGL_FIXED_WIDTH_NOK || a[0] == EGL_FIXED_HEIGHT_NOK)
			continue;
		clean[n++] = a[0];
		clean[n++] = a[1];
	}
	clean[n] = EGL_NONE;
	return real_CreateWindowSurface(dpy, config, win, attrs ? clean : NULL);
}

static const char nok_exts[] = " EGL_NOK_image_shared EGL_NOK_surface_scaling EGL_KHR_lock_surface EGL_KHR_lock_surface2";

EXPORT const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
	static struct { EGLDisplay dpy; char *s; } cache[8];
	INIT();
	const char *s = real_QueryString(dpy, name);
	if (name != EGL_EXTENSIONS || dpy == EGL_NO_DISPLAY || !s ||
	    !strstr(s, "EGL_MESA_image_dma_buf_export") || !strstr(s, "EGL_EXT_image_dma_buf_import"))
		return s;

	pthread_mutex_lock(&lock);
	const char *ret = s;
	for (int i = 0; i < 8; i++) {
		if (cache[i].dpy == dpy && cache[i].s) {
			ret = cache[i].s;
			break;
		}
		if (!cache[i].s) {
			char *e = malloc(strlen(s) + sizeof(nok_exts));
			if (e) {
				strcpy(e, s);
				strcat(e, nok_exts);
				cache[i].dpy = dpy;
				cache[i].s = e;
				ret = e;
			}
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name)
{
	static const struct { const char *name; void *fn; } own[] = {
		{ "eglCreateImageKHR", (void *)eglCreateImageKHR },
		{ "eglDestroyImageKHR", (void *)eglDestroyImageKHR },
		{ "eglCreateSharedImageNOK", (void *)eglCreateSharedImageNOK },
		{ "eglDestroySharedImageNOK", (void *)eglDestroySharedImageNOK },
		{ "eglQueryImageNOK", (void *)eglQueryImageNOK },
		{ "eglSetSurfaceScalingNOK", (void *)eglSetSurfaceScalingNOK },
		{ "eglCreateWindowSurface", (void *)eglCreateWindowSurface },
		{ "eglQueryString", (void *)eglQueryString },
		{ "eglCreatePixmapSurface", (void *)eglCreatePixmapSurface },
		{ "eglDestroySurface", (void *)eglDestroySurface },
		{ "eglQuerySurface", (void *)eglQuerySurface },
		{ "eglLockSurfaceKHR", (void *)eglLockSurfaceKHR },
		{ "eglUnlockSurfaceKHR", (void *)eglUnlockSurfaceKHR },
	};
	INIT();
	for (size_t i = 0; name && i < sizeof(own) / sizeof(own[0]); i++)
		if (!strcmp(name, own[i].name))
			return (__eglMustCastToProperFunctionPointerType)own[i].fn;
	return real_GetProcAddress(name);
}
