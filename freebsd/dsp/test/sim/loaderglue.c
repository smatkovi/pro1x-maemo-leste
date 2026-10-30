/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The loader pro1x-dsp (tools/pro1x-dsp) runs inside the simulation:
 * compiled with its open, close, ioctl, err, warn, exit and main renamed
 * (Makefile), its /dev/qcom_pas/<name> is the driver's character device,
 * the firmware files are real ones, and its exit returns to the caller.
 */

#include "simkern.h"
#include "sim.h"

#include <err.h>
#include <fcntl.h>
#include <setjmp.h>
#include <unistd.h>

#undef printf
#undef malloc
#undef free

int	pro1x_dsp_main(int argc, char **argv);

static jmp_buf jb;
static int rc;
static struct cdev *fds[16];

void __attribute__((noreturn))
sim_exit(int code)
{
	rc = code;
	longjmp(jb, 1);
}

static void
vmsg(const char *fmt, va_list ap, bool with_errno)
{
	char buf[512];
	int e = errno;

	vsnprintf(buf, sizeof(buf), fmt != NULL ? fmt : "", ap);
	if (with_errno)
		sim_printf("pro1x-dsp: %s: %s\n", buf, strerror(e));
	else
		sim_printf("pro1x-dsp: %s\n", buf);
}

void __attribute__((noreturn))
sim_err(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(fmt, ap, true);
	va_end(ap);
	sim_exit(eval);
}

void __attribute__((noreturn))
sim_errx(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(fmt, ap, false);
	va_end(ap);
	sim_exit(eval);
}

void
sim_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(fmt, ap, true);
	va_end(ap);
}

void
sim_warnx(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(fmt, ap, false);
	va_end(ap);
}

int
sim_open(const char *path, int flags, ...)
{
	struct cdev *c;
	int i;

	if (strncmp(path, "/dev/", 5) != 0)
		return (open(path, flags));
	if ((c = sim_cdev_find(path + 5)) == NULL) {
		errno = ENOENT;
		return (-1);
	}
	for (i = 0; i < 16 && fds[i] != NULL; i++)
		;
	if (i == 16) {
		errno = EMFILE;
		return (-1);
	}
	fds[i] = c;
	return (1000 + i);
}

int
sim_close(int fd)
{
	if (fd >= 1000 && fd < 1016) {
		fds[fd - 1000] = NULL;
		return (0);
	}
	return (close(fd));
}

int
sim_ioctl(int fd, unsigned long cmd, ...)
{
	struct thread td;
	struct cdev *c;
	va_list ap;
	void *arg;
	int error;

	va_start(ap, cmd);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd < 1000 || fd >= 1016 || (c = fds[fd - 1000]) == NULL) {
		errno = EBADF;
		return (-1);
	}
	error = c->si_devsw->d_ioctl(c, cmd, arg, 0, &td);
	if (error != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

/* pro1x-dsp ARGS...: its exit status */
int
sim_loader(int argc, char **argv)
{
	if (setjmp(jb) == 0)
		rc = pro1x_dsp_main(argc, argv);
	return (rc);
}
