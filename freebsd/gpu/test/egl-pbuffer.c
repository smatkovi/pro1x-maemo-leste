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
	static const EGLint pb_attr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat tri[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;
	EGLContext ctx;
	EGLint n, major, minor;
	GLuint prog;
	unsigned char mid[4], corner[4];
	int ok;

	if (argc == 3 && strcmp(argv[1], "gbm") == 0)
		return gbm_test(argv[2]);

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
	    eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (get_display == NULL) {
		fprintf(stderr, "no eglGetPlatformDisplayEXT\n");
		return 1;
	}
	dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "eglInitialize: 0x%x\n", eglGetError());
		return 1;
	}
	printf("EGL %d.%d, %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "no pbuffer config: 0x%x\n", eglGetError());
		return 1;
	}
	surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
	eglBindAPI(EGL_OPENGL_ES_API);
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "pbuffer/context: 0x%x\n", eglGetError());
		return 1;
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
	eglDestroySurface(dpy, surf);
	eglTerminate(dpy);
	return ok ? 0 : 1;
}
