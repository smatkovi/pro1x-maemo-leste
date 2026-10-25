/*
 * Small differences between the N9 and the Pro1X that Harmattan programs
 * stumble over. Loaded into every Harmattan process through
 * /etc/ld.so.preload (38-hardware-adaptation.sh); each override only takes
 * effect in the program that calls the function through its PLT.
 */
#include <stdbool.h>

#define EXPORT __attribute__((visibility("default")))

/*
 * MafwMmcMonitor::isMounted() - the media player (qmafw's GStreamer
 * renderer) plays nothing from /home/user/MyDocs while it is not mounted:
 * on the N9 that was its own FAT partition, unmounted for USB mass
 * storage. It looks for a GIO mount there, and GIO ignores bind mounts.
 * On the Pro1X MyDocs is a directory and always there.
 */
EXPORT bool _ZN14MafwMmcMonitor9isMountedEv(void *self)
{
	(void)self;
	return true;
}
