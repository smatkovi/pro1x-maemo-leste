/* evsend TYPE CODE VALUE: one input event into the FIFO /dev/input/event0
 * (the test's private /dev/input), which mce reads as a keyboard */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct input_event ev;
	struct timeval tv;
	int fd;

	if (argc != 4)
		return 2;
	if ((fd = open("/dev/input/event0", O_WRONLY | O_NONBLOCK)) < 0) {
		perror("event0");
		return 1;
	}
	memset(&ev, 0, sizeof(ev));
	gettimeofday(&tv, NULL);
	ev.input_event_sec = tv.tv_sec;
	ev.input_event_usec = tv.tv_usec;
	ev.type = atoi(argv[1]);
	ev.code = atoi(argv[2]);
	ev.value = atoi(argv[3]);
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
		perror("write");
		return 1;
	}
	close(fd);
	return 0;
}
