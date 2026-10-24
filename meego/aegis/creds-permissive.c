/*
 * libcreds.so.2 without the Aegis kernel: every credential check passes.
 *
 * Harmattan's security framework (Aegis) keeps "resource tokens" such as
 * applauncherd-launcher::access in a kernel module of the N9. Without it
 * libcreds falls back to /proc and knows no tokens, so applauncherd refuses
 * every invoker ("Security credential check failed") and other daemons
 * reject their clients. Nokia's own libcreds2-audit build answers "yes" to
 * all checks and logs the ones that would have failed; this is the same
 * build of libcreds2's creds.c, without the logging (the audit build forks
 * a syslog child per failed check).
 *
 * Build: in the unpacked libcreds2 source, see meego/36-aegis-permissive.sh
 */
#define _ISOC99_SOURCE
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/creds.h>
#undef _ISOC99_SOURCE
#undef _GNU_SOURCE

#define CREDS_AUDIT_LOG

static void creds_audit_init(creds_t creds, pid_t pid);
static void creds_audit_free(creds_t creds);
static void creds_audit_log(creds_t creds, creds_type_t type, creds_value_t value);

typedef struct {
	char *name;
} creds_audit_t;

#include "creds.c"

static void creds_audit_init(creds_t creds, pid_t pid)
{
	(void)pid;
	creds->audit.name = NULL;
}

static void creds_audit_free(creds_t creds)
{
	(void)creds;
}

static void creds_audit_log(creds_t creds, creds_type_t type, creds_value_t value)
{
	(void)creds; (void)type; (void)value;
}

/*
 * Policy loading and confinement (credpctl.c) need the kernel module too;
 * applauncherd's boosters refuse to start an application when confining it
 * fails. Without the module there is nothing to confine: report success.
 */
long credp_kload(int flags, const char *path, const __u32 *list, size_t list_length)
{
	(void)flags; (void)path; (void)list; (void)list_length;
	return 0;
}

long credp_kunload(const char *path)
{
	(void)path;
	return 0;
}

long credp_kconfine(const char *path)
{
	(void)path;
	return 0;
}

long credp_kconfine2(const char *path, int flags, const __u32 *list, size_t list_length)
{
	(void)path; (void)flags; (void)list; (void)list_length;
	return 0;
}

long credp_kset(int flags, const __u32 *list, size_t list_length)
{
	(void)flags; (void)list; (void)list_length;
	return 0;
}
