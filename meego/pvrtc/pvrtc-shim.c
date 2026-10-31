/*
 * PVRTC textures for N9 games on Mesa.
 *
 * The N9's PowerVR SGX had GL_IMG_texture_compression_pvrtc, and games for
 * it ship their textures in that format (NFS Shift: 750 of its 1200 M3G
 * images are PVRTC 4bpp). Adreno and Mesa have no PVRTC, so those textures
 * stay empty. This library stands in for the GLES library a game opens
 * (built twice: as libGLES_CM.so over Debian's libGLESv1_CM.so.1 and as
 * libGLESv2.so over libGLESv2.so.2, the SGX driver's unversioned names) and
 * is found first through LD_LIBRARY_PATH=/usr/lib/pro1x/pvrtc, which
 * pvrtc-run sets for the games that need it. Everything else comes from the
 * real library, a DT_NEEDED of this one, so dlsym() on our handle and plain
 * linking both reach it.
 *
 *  glGetString(GL_EXTENSIONS)       + GL_IMG_texture_compression_pvrtc
 *  glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS / _FORMATS)  + 4 formats
 *  glCompressedTex(Sub)Image2D      PVRTC 2/4bpp decoded (pvrtc-decode.c),
 *                                   uploaded as RGBA8 with glTex(Sub)Image2D
 *
 * PVRTC_TRACE=1 logs to stderr what the program asks and uploads, and GL
 * errors after the uploads.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES2/gl2.h>

#include "pvrtc-decode.h"

#ifndef REAL_LIB
#define REAL_LIB "libGLESv2.so.2"
#endif

#define EXPORT __attribute__((visibility("default")))

#define RGB_4BPP	0x8C00	/* GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG */
#define RGB_2BPP	0x8C01
#define RGBA_4BPP	0x8C02
#define RGBA_2BPP	0x8C03
#define EXT_NAME	"GL_IMG_texture_compression_pvrtc"

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int trace;

static const GLubyte *(*real_GetString)(GLenum);
static void (*real_GetIntegerv)(GLenum, GLint *);
static GLenum (*real_GetError)(void);
static void (*real_CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static void (*real_CompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
static void (*real_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*real_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*real_PixelStorei)(GLenum, GLint);

static void init(void)
{
	void *h = dlopen(REAL_LIB, RTLD_NOW | RTLD_LOCAL);
	const char *t = getenv("PVRTC_TRACE");
	trace = t && *t && *t != '0';
	if (!h) {
		fprintf(stderr, "pvrtc: %s: %s\n", REAL_LIB, dlerror());
		abort();
	}
	real_GetString = dlsym(h, "glGetString");
	real_GetIntegerv = dlsym(h, "glGetIntegerv");
	real_GetError = dlsym(h, "glGetError");
	real_CompressedTexImage2D = dlsym(h, "glCompressedTexImage2D");
	real_CompressedTexSubImage2D = dlsym(h, "glCompressedTexSubImage2D");
	real_TexImage2D = dlsym(h, "glTexImage2D");
	real_TexSubImage2D = dlsym(h, "glTexSubImage2D");
	real_PixelStorei = dlsym(h, "glPixelStorei");
	if (trace)
		fprintf(stderr, "pvrtc: over %s\n", REAL_LIB);
}

#define INIT() pthread_once(&once, init)

/* An error our own calls produced, owed to the program's next glGetError. */
static GLenum owed;

static void check(const char *what, GLenum fmt, int w, int h)
{
	GLenum e = real_GetError();
	if (!e)
		return;
	if (!owed)
		owed = e;
	if (trace)
		fprintf(stderr, "pvrtc: %s (0x%04x %dx%d): GL error 0x%04x\n", what, fmt, w, h, e);
}

EXPORT GLenum glGetError(void)
{
	INIT();
	GLenum e = owed;
	if (e) {
		owed = 0;
		return e;
	}
	return real_GetError();
}

/* ---- the extension -------------------------------------------------- */

struct ext_cache {
	char *orig, *ours;
};
static struct ext_cache exts[8];

EXPORT const GLubyte *glGetString(GLenum name)
{
	INIT();
	const GLubyte *s = real_GetString(name);
	if (name != GL_EXTENSIONS || !s || strstr((const char *)s, EXT_NAME))
		return s;
	const char *ret = (const char *)s;
	pthread_mutex_lock(&lock);
	int i;
	for (i = 0; i < 8 && exts[i].orig; i++)
		if (!strcmp(exts[i].orig, (const char *)s))
			break;
	if (i < 8 && exts[i].orig) {
		ret = exts[i].ours;
	} else {
		if (i == 8) {	/* full: drop the oldest */
			free(exts[0].orig);
			free(exts[0].ours);
			memmove(exts, exts + 1, sizeof(exts[0]) * 7);
			i = 7;
		}
		size_t n = strlen((const char *)s);
		char *o = strdup((const char *)s), *x = malloc(n + sizeof(EXT_NAME) + 2);
		if (o && x) {
			memcpy(x, s, n);
			if (n && x[n - 1] != ' ')
				x[n++] = ' ';
			strcpy(x + n, EXT_NAME " ");
			exts[i] = (struct ext_cache){ o, x };
			ret = x;
			if (trace)
				fprintf(stderr, "pvrtc: GL_EXTENSIONS was: %s\n", o);
		} else {
			free(o);
			free(x);
		}
	}
	pthread_mutex_unlock(&lock);
	return (const GLubyte *)ret;
}

EXPORT void glGetIntegerv(GLenum pname, GLint *v)
{
	INIT();
	if (pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS) {
		real_GetIntegerv(pname, v);
		if (trace)
			fprintf(stderr, "pvrtc: NUM_COMPRESSED_TEXTURE_FORMATS %d + 4\n", v[0]);
		v[0] += 4;
		return;
	}
	if (pname == GL_COMPRESSED_TEXTURE_FORMATS) {
		GLint n = 0;
		real_GetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
		real_GetIntegerv(pname, v);
		v[n] = RGB_4BPP;
		v[n + 1] = RGB_2BPP;
		v[n + 2] = RGBA_4BPP;
		v[n + 3] = RGBA_2BPP;
		return;
	}
	real_GetIntegerv(pname, v);
}

/* ---- the textures ---------------------------------------------------- */

static int pvrtc_bpp(GLenum f)
{
	switch (f) {
	case RGB_4BPP:
	case RGBA_4BPP:
		return 4;
	case RGB_2BPP:
	case RGBA_2BPP:
		return 2;
	}
	return 0;
}

/* Decode; NULL when the arguments are not a valid PVRTC level (the real
 * call then reports the error, as the SGX driver would have). */
static uint8_t *decode(GLenum fmt, GLsizei w, GLsizei h, GLsizei size, const void *data)
{
	int bpp = pvrtc_bpp(fmt);
	if (w <= 0 || h <= 0 || !data || (size_t)size < pvrtc_size(w, h, bpp))
		return NULL;
	uint8_t *rgba = malloc((size_t)w * h * 4);
	if (rgba && pvrtc_decode(data, w, h, bpp, fmt == RGB_4BPP || fmt == RGB_2BPP, rgba)) {
		free(rgba);
		rgba = NULL;
	}
	return rgba;
}

/* RGBA rows are multiples of 4 bytes; only an unpack alignment of 8 would
 * pad them. */
static GLint align_begin(void)
{
	GLint a = 4;
	real_GetIntegerv(GL_UNPACK_ALIGNMENT, &a);
	if (a > 4)
		real_PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	return a;
}

static void align_end(GLint a)
{
	if (a > 4)
		real_PixelStorei(GL_UNPACK_ALIGNMENT, a);
}

static unsigned long ntex;

EXPORT void glCompressedTexImage2D(GLenum target, GLint level, GLenum fmt, GLsizei w, GLsizei h,
				   GLint border, GLsizei size, const void *data)
{
	INIT();
	uint8_t *rgba = pvrtc_bpp(fmt) && !border ? decode(fmt, w, h, size, data) : NULL;
	if (!rgba) {
		real_CompressedTexImage2D(target, level, fmt, w, h, border, size, data);
		if (trace)
			check("glCompressedTexImage2D, passed on", fmt, w, h);
		return;
	}
	GLint a = align_begin();
	real_TexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	align_end(a);
	free(rgba);
	if (trace) {
		check("glCompressedTexImage2D", fmt, w, h);
		unsigned long n = __atomic_add_fetch(&ntex, 1, __ATOMIC_RELAXED);
		if ((n & (n - 1)) == 0)
			fprintf(stderr, "pvrtc: %lu levels decoded (last 0x%04x %dx%d level %d)\n",
				n, fmt, w, h, level);
	}
}

EXPORT void glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
				      GLsizei h, GLenum fmt, GLsizei size, const void *data)
{
	INIT();
	uint8_t *rgba = pvrtc_bpp(fmt) ? decode(fmt, w, h, size, data) : NULL;
	if (!rgba) {
		real_CompressedTexSubImage2D(target, level, x, y, w, h, fmt, size, data);
		if (trace)
			check("glCompressedTexSubImage2D, passed on", fmt, w, h);
		return;
	}
	GLint a = align_begin();
	real_TexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	align_end(a);
	free(rgba);
	if (trace)
		check("glCompressedTexSubImage2D", fmt, w, h);
}
