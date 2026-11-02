/*
 * GLES 2 through EGL without a window system (EGL_MESA_platform_surfaceless,
 * as Mesa has it): a 64 x 64 pbuffer cleared blue, a red triangle in its
 * middle, read back with glReadPixels. Prints the renderer and the Mesa
 * libraries the process loaded (freebsd/gpu/mesa.sh test runs it on the
 * staged ones).
 *
 *   egl-pbuffer             render; exit 0 when the pixels are right
 *   egl-pbuffer gbm FILE    gbm_create_device() on FILE: on a file that is
 *                           no GPU the driver has to refuse, not crash
 *   egl-pbuffer render FILE the same drawing through EGL on GBM on FILE
 *                           (/dev/dri/renderD128: freedreno found by the
 *                           device's driver name, not by libdrm's device
 *                           list, which has no msm on FreeBSD), into a
 *                           framebuffer object of a context without surface
 *   egl-pbuffer x11         the same on the X server ($DISPLAY): a client
 *                           of X, its buffers through DRI3 when the server
 *                           offers it (Xorg's modesetting with glamor)
 * (freebsd/gpu/msm-port.md run gpu4, kms.md run kms3)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <gbm.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>

static void
where(const char *lib)
{
	void *h = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD);
	struct link_map *m;

	if (h != NULL && dlinfo(h, RTLD_DI_LINKMAP, &m) == 0)
		printf("loaded: %s\n", m->l_name);
	else
		printf("not loaded: %s\n", lib);
}

static int
gbm_test(const char *path)
{
	struct gbm_device *gbm;
	int fd = open(path, O_RDWR);

	if (fd < 0) {
		perror(path);
		return 1;
	}
	gbm = gbm_create_device(fd);
	where("libgallium-26.2.2.so");
	if (gbm == NULL) {
		printf("gbm_create_device(%s): NULL (refused)\n", path);
	} else {
		printf("gbm_create_device(%s): backend %s\n", path,
		    gbm_device_get_backend_name(gbm));
		gbm_device_destroy(gbm);
	}
	close(fd);
	return 0;
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
	}
	return s;
}

int
main(int argc, char **argv)
{
	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_NONE
	};
	static const EGLint cfg_any[] = {
		EGL_SURFACE_TYPE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE
	};
	static const EGLint pb_attr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat tri[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	EGLint n, major, minor;
	EGLenum platform = EGL_PLATFORM_SURFACELESS_MESA;
	void *native = EGL_DEFAULT_DISPLAY;
	struct gbm_device *gbm = NULL;
	Display *x = NULL;
	GLuint prog, tex, fbo;
	unsigned char mid[4], corner[4];
	int ok, fd, pbuffer = 1;

	if (argc == 3 && strcmp(argv[1], "gbm") == 0)
		return gbm_test(argv[2]);
	if (argc == 3 && strcmp(argv[1], "render") == 0) {
		if ((fd = open(argv[2], O_RDWR)) < 0 ||
		    (gbm = gbm_create_device(fd)) == NULL) {
			fprintf(stderr, "no GBM device on %s\n", argv[2]);
			return 1;
		}
		printf("GBM on %s: backend %s\n", argv[2],
		    gbm_device_get_backend_name(gbm));
		platform = EGL_PLATFORM_GBM_KHR;
		native = gbm;
		pbuffer = 0;
	} else if (argc == 2 && strcmp(argv[1], "x11") == 0) {
		if ((x = XOpenDisplay(NULL)) == NULL) {
			fprintf(stderr, "no X display\n");
			return 1;
		}
		platform = EGL_PLATFORM_X11_KHR;
		native = x;
		pbuffer = 0;
	}

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
	    eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (get_display == NULL) {
		fprintf(stderr, "no eglGetPlatformDisplayEXT\n");
		return 1;
	}
	dpy = get_display(platform, native, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "eglInitialize: 0x%x\n", eglGetError());
		return 1;
	}
	printf("EGL %d.%d, %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));
	if (!eglChooseConfig(dpy, pbuffer ? cfg_attr : cfg_any, &cfg, 1, &n) ||
	    n < 1) {
		fprintf(stderr, "no %s config: 0x%x\n", pbuffer ? "pbuffer" :
		    "GLES 2", eglGetError());
		return 1;
	}
	surf = pbuffer ? eglCreatePbufferSurface(dpy, cfg, pb_attr) :
	    EGL_NO_SURFACE;
	eglBindAPI(EGL_OPENGL_ES_API);
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if ((pbuffer && surf == EGL_NO_SURFACE) || ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "pbuffer/context: 0x%x\n", eglGetError());
		return 1;
	}
	if (!pbuffer) {
		/* no surface (EGL_KHR_surfaceless_context): a texture as
		 * the colour buffer */
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
		    GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_TEXTURE_2D, tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
		    GL_FRAMEBUFFER_COMPLETE) {
			fprintf(stderr, "framebuffer object incomplete\n");
			return 1;
		}
	}
	printf("GL_RENDERER: %s\nGL_VERSION: %s\n",
	    (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_VERSION));
	where("libEGL_mesa.so.0");
	where("libgallium-26.2.2.so");

	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER,
	    "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }"));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	glViewport(0, 0, 64, 64);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, mid);
	glReadPixels(1, 62, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
	ok = mid[0] == 255 && mid[1] == 0 && mid[2] == 0 &&
	    corner[0] == 0 && corner[1] == 0 && corner[2] == 255;
	printf("middle %d,%d,%d corner %d,%d,%d: %s\n", mid[0], mid[1], mid[2],
	    corner[0], corner[1], corner[2], ok ? "OK" : "WRONG");
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	if (surf != EGL_NO_SURFACE)
		eglDestroySurface(dpy, surf);
	eglTerminate(dpy);
	if (gbm != NULL)
		gbm_device_destroy(gbm);
	if (x != NULL)
		XCloseDisplay(x);
	return ok ? 0 : 1;
}
