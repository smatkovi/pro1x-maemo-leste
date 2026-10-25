/*
 * aegis-exec without the Aegis kernel module.
 *
 * Harmattan's session jobs (upstart files in /etc/init/xsession) start
 * their daemons as  aegis-exec -s -u user -l "exec /usr/bin/daemon ...".
 * The original
 * sets the process credentials through the N9's Aegis module and crashes
 * without it. This one only does what remains meaningful: switch to the
 * user, with the supplementary groups the MeeGo session passes in
 * PRO1X_GROUPS (comma-separated gids, as 40-harmattan-session.sh computes
 * them), and run the command.
 *
 *   aegis-exec [-s] [-u user] [-a ...] [-l] command...
 *
 * -l: the command is a shell command line; otherwise it is executed as is.
 * Other options of the original (resource tokens, credential sets) have no
 * meaning here and are skipped.
 */
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int set_groups(gid_t primary)
{
	gid_t list[64];
	int n = 0;
	const char *env = getenv("PRO1X_GROUPS");

	list[n++] = primary;
	for (const char *p = env; p && *p && n < 64; ) {
		char *end;
		long g = strtol(p, &end, 10);
		if (end == p)
			break;
		list[n++] = (gid_t)g;
		p = *end == ',' ? end + 1 : end;
	}
	return setgroups(n, list);
}

int main(int argc, char **argv)
{
	const char *user = NULL;
	int shell = 0, i;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-u") && i + 1 < argc)
			user = argv[++i];
		else if (!strcmp(argv[i], "-l"))
			shell = 1;
		else if (!strcmp(argv[i], "--")) {
			i++;
			break;
		} else if ((!strcmp(argv[i], "-a") || !strcmp(argv[i], "-r") ||
			    !strcmp(argv[i], "-c")) && i + 1 < argc)
			i++;	/* resource token / credential: no Aegis */
	}
	if (i >= argc) {
		fprintf(stderr, "usage: aegis-exec [-s] [-u user] [-l] command...\n");
		return 2;
	}

	if (user && getuid() == 0) {
		struct passwd *pw = getpwnam(user);
		if (!pw) {
			fprintf(stderr, "aegis-exec: no user %s\n", user);
			return 1;
		}
		if (set_groups(pw->pw_gid) || setgid(pw->pw_gid) || setuid(pw->pw_uid)) {
			perror("aegis-exec");
			return 1;
		}
		setenv("HOME", pw->pw_dir, 1);
		setenv("USER", pw->pw_name, 1);
		setenv("LOGNAME", pw->pw_name, 1);
	}

	if (shell) {
		execl("/bin/sh", "sh", "-c", argv[i], (char *)NULL);
	} else {
		execvp(argv[i], argv + i);
	}
	perror(argv[i]);
	return 127;
}
