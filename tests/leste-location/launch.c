/*
 * Runs a maemo-launcher program (a .launch, a shared object without an
 * interpreter) without the launcher: dlopen()s it and calls its main(),
 * as maemo-launcher does after its fork.
 *
 *   gcc -o launch launch.c -ldl;  launch /usr/libexec/location-ui.launch [ARGS]
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	void *h;
	int (*program_main)(int, char **);

	if (argc < 2) {
		fprintf(stderr, "usage: %s FILE.launch [ARGS]\n", argv[0]);
		return 2;
	}
	h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
	if (!h) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	program_main = (int (*)(int, char **))dlsym(h, "main");
	if (!program_main) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	return program_main(argc - 1, argv + 1);
}
