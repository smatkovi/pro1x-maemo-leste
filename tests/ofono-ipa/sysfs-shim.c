/*
 * LD_PRELOAD for the oFono IPA test: a dummy rmnet_ipa0 has no IPA sysfs
 * files, so access() and open() of
 *
 *     /sys/.../rmnet_ipa0/device/<file>
 *
 * (udevng's mainline IPA check, qrtrqmi's endpoint ids and offload names)
 * go to $FAKE_DEVICE/<file> instead. Everything else passes through.
 *
 *     gcc -shared -fPIC -O1 -Wall -o sysfs-shim.so sysfs-shim.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MARK "/rmnet_ipa0/device/"

static const char *redirect(const char *path, char *buf, size_t len)
{
	const char *fake = getenv("FAKE_DEVICE");
	const char *p = path ? strstr(path, MARK) : NULL;

	if (!fake || !p || strncmp(path, "/sys/", 5))
		return path;

	snprintf(buf, len, "%s/%s", fake, p + strlen(MARK));
	return buf;
}

int access(const char *path, int mode)
{
	static int (*real)(const char *, int);
	char buf[512];

	if (!real)
		real = (int (*)(const char *, int)) dlsym(RTLD_NEXT, "access");

	return real(redirect(path, buf, sizeof(buf)), mode);
}

#define OPEN(name)							\
int name(const char *path, int flags, ...)				\
{									\
	static int (*real)(const char *, int, ...);			\
	char buf[512];							\
	mode_t mode = 0;						\
									\
	if (flags & O_CREAT) {						\
		va_list ap;						\
									\
		va_start(ap, flags);					\
		mode = va_arg(ap, mode_t);				\
		va_end(ap);						\
	}								\
									\
	if (!real)							\
		real = (int (*)(const char *, int, ...))		\
			dlsym(RTLD_NEXT, #name);			\
									\
	return real(redirect(path, buf, sizeof(buf)), flags, mode);	\
}

OPEN(open)
OPEN(open64)
