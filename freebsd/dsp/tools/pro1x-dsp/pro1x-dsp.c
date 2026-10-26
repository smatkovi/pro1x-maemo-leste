/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-dsp: starts and stops the Pro1X's remote processors under FreeBSD
 * (kernel module qcom_dsp, freebsd/dsp/README.md).
 *
 *   pro1x-dsp start NAME [FIRMWARE]   NAME: adsp, cdsp or modem
 *   pro1x-dsp stop NAME
 *   pro1x-dsp status [NAME]
 *   pro1x-dsp -n start NAME FIRMWARE   only show what would be loaded
 *
 * The firmware is a signed ELF image (.mbn, or .mdt with .bNN segment
 * files), read as Linux's mdt_loader reads it: TrustZone gets the ELF and
 * program headers with the hash segment (the metadata), the loadable
 * segments go into the processor's memory region, relocated to its start
 * when the image says so, then TrustZone checks the image and starts the
 * processor. Default firmware: FWDIR/<name>.mbn.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qcom_pas_ioctl.h"

#define	FWDIR		"/usr/local/share/pro1x/firmware/qcom/sm6115/Fxtec/QX1050"
#define	START_MS	5000

/* ELF32, as the firmware images are */
struct elf32_ehdr {
	uint8_t		e_ident[16];
	uint16_t	e_type, e_machine;
	uint32_t	e_version, e_entry, e_phoff, e_shoff, e_flags;
	uint16_t	e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
			e_shstrndx;
};

struct elf32_phdr {
	uint32_t	p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
			p_flags, p_align;
};

#define	PT_LOAD			1
#define	MDT_TYPE_MASK		(7U << 24)
#define	MDT_TYPE_HASH		(2U << 24)
#define	MDT_RELOCATABLE		(1U << 27)

static const char *states[] = { "off", "loading", "running", "crashed" };
static int dry;		/* -n: no device, only the plan */

static int
open_dev(const char *name)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/dev/qcom_pas/%s", name);
	if ((fd = open(path, O_RDWR)) < 0)
		err(1, "%s (kldload qcom_dsp?)", path);
	return (fd);
}

static int
loadable(const struct elf32_phdr *ph)
{
	return (ph->p_type == PT_LOAD &&
	    (ph->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && ph->p_memsz != 0);
}

/* A segment beyond the .mdt file comes from FILE.bNN */
static void *
split_segment(const char *fw, int i, size_t size)
{
	char path[1024];
	struct stat st;
	void *buf;
	size_t len;
	int fd;

	len = strlen(fw);
	if (len < 4 || len + 1 > sizeof(path))
		errx(1, "%s: segment %d is not in the file", fw, i);
	memcpy(path, fw, len - 3);
	snprintf(path + len - 3, sizeof(path) - (len - 3), "b%02d", i);
	if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, &st) < 0)
		err(1, "%s", path);
	if ((size_t)st.st_size != size)
		errx(1, "%s: %jd bytes, the image says %zu", path,
		    (intmax_t)st.st_size, size);
	if ((buf = malloc(size)) == NULL || read(fd, buf, size) != (ssize_t)size)
		err(1, "%s", path);
	close(fd);
	return (buf);
}

static void
status(const char *name)
{
	struct qcom_pas_status st;
	int fd;

	fd = open_dev(name);
	if (ioctl(fd, QCOM_PAS_STATUS, &st) < 0)
		err(1, "%s: status", name);
	close(fd);
	printf("%s: %s, PAS id %u, memory %#jx+%#jx, SMP2P %#x, seen%s%s%s%s%s",
	    name, st.state < 4 ? states[st.state] : "?", st.pas_id,
	    (uintmax_t)st.mem_phys, (uintmax_t)st.mem_size, st.smp2p_in,
	    st.flags & QCOM_PAS_F_READY ? " ready" : "",
	    st.flags & QCOM_PAS_F_HANDOVER ? " handover" : "",
	    st.flags & QCOM_PAS_F_FATAL ? " fatal" : "",
	    st.flags & QCOM_PAS_F_WDOG ? " watchdog" : "",
	    st.flags & QCOM_PAS_F_STOP_ACK ? " stop-ack" : "");
	if (st.crash_reason[0] != '\0')
		printf(", crash: %s", st.crash_reason);
	printf("\n");
}

static void
start(const char *name, const char *fw)
{
	const struct elf32_ehdr *eh;
	const struct elf32_phdr *ph;
	struct qcom_pas_status st;
	struct qcom_pas_init init;
	struct qcom_pas_segment seg;
	uint64_t min = UINT64_MAX, max = 0, reloc, size;
	uint32_t timeout = START_MS;
	size_t ehdr_size, hash_size;
	struct stat sb;
	char *meta;
	const char *img;
	void *data;
	int fd, dev, i, hash = 0, relocate = 0;

	if ((fd = open(fw, O_RDONLY)) < 0 || fstat(fd, &sb) < 0)
		err(1, "%s", fw);
	img = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (img == MAP_FAILED)
		err(1, "%s", fw);
	eh = (const struct elf32_ehdr *)img;
	if ((size_t)sb.st_size < sizeof(*eh) || memcmp(eh->e_ident, "\177ELF", 4) != 0 ||
	    eh->e_ident[4] != 1 || eh->e_phentsize != sizeof(*ph) ||
	    eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(*ph) > (uint64_t)sb.st_size ||
	    eh->e_phnum < 2)
		errx(1, "%s: not a 32-bit firmware ELF image", fw);
	ph = (const struct elf32_phdr *)(img + eh->e_phoff);
	if (ph[0].p_type == PT_LOAD)
		errx(1, "%s: no header segment", fw);
	for (i = 1; i < eh->e_phnum; i++)
		if ((ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			hash = i;
			break;
		}
	if (hash == 0)
		errx(1, "%s: no hash segment", fw);
	for (i = 0; i < eh->e_phnum; i++) {
		if (!loadable(&ph[i]))
			continue;
		if (ph[i].p_flags & MDT_RELOCATABLE)
			relocate = 1;
		if (ph[i].p_paddr < min)
			min = ph[i].p_paddr;
		if (ph[i].p_paddr + (uint64_t)ph[i].p_memsz > max)
			max = (ph[i].p_paddr + (uint64_t)ph[i].p_memsz + 4095) &
			    ~(uint64_t)4095;
	}
	if (min >= max)
		errx(1, "%s: no loadable segment", fw);

	/* the metadata: ELF header and program headers, then the hash */
	ehdr_size = ph[0].p_filesz;
	hash_size = ph[hash].p_filesz;
	if (ehdr_size > (size_t)sb.st_size)
		errx(1, "%s: truncated", fw);
	if ((meta = malloc(ehdr_size + hash_size)) == NULL)
		err(1, NULL);
	memcpy(meta, img, ehdr_size);
	if (ehdr_size + hash_size == (size_t)sb.st_size)
		memcpy(meta + ehdr_size, img + ehdr_size, hash_size);
	else if (ph[hash].p_offset + (uint64_t)hash_size <= (uint64_t)sb.st_size)
		memcpy(meta + ehdr_size, img + ph[hash].p_offset, hash_size);
	else {
		data = split_segment(fw, hash, hash_size);
		memcpy(meta + ehdr_size, data, hash_size);
		free(data);
	}

	memset(&st, 0, sizeof(st));
	st.mem_size = UINT64_MAX;
	dev = dry ? -1 : open_dev(name);
	if (!dry && ioctl(dev, QCOM_PAS_STATUS, &st) < 0)
		err(1, "%s: status", name);
	if (st.state == QCOM_PAS_STATE_RUNNING)
		errx(1, "%s is running (pro1x-dsp stop %s)", name, name);
	if (max - min > st.mem_size)
		errx(1, "%s: %ju bytes, the memory region has %ju", fw,
		    (uintmax_t)(max - min), (uintmax_t)st.mem_size);
	printf("%s: %s, %d segments, %ju KB%s\n", name, fw, eh->e_phnum,
	    (uintmax_t)(max - min) / 1024, relocate ? ", relocatable" : "");

	init.meta = (uintptr_t)meta;
	init.meta_len = ehdr_size + hash_size;
	if (dry)
		printf("metadata: %zu bytes (headers %zu, hash %zu)%s\n",
		    ehdr_size + hash_size, ehdr_size, hash_size,
		    relocate ? ", memory setup" : "");
	else if (ioctl(dev, QCOM_PAS_INIT, &init) < 0)
		err(1, "%s: TrustZone did not take the metadata", name);
	if (relocate) {
		size = max - min;
		if (!dry && ioctl(dev, QCOM_PAS_MEM_SETUP, &size) < 0)
			err(1, "%s: memory setup", name);
	}
	reloc = relocate ? min : st.mem_phys;
	for (i = 0; i < eh->e_phnum; i++) {
		if (!loadable(&ph[i]))
			continue;
		if (ph[i].p_paddr < reloc)
			errx(1, "%s: segment %d below the memory region", fw, i);
		seg.offset = ph[i].p_paddr - reloc;
		seg.filesz = ph[i].p_filesz;
		seg.memsz = ph[i].p_memsz;
		data = NULL;
		if (seg.filesz == 0)
			seg.data = 0;
		else if (ph[i].p_offset + (uint64_t)ph[i].p_filesz <=
		    (uint64_t)sb.st_size)
			seg.data = (uintptr_t)(img + ph[i].p_offset);
		else {
			data = split_segment(fw, i, ph[i].p_filesz);
			seg.data = (uintptr_t)data;
		}
		if (dry)
			printf("segment %2d: offset %#9jx, %#8jx bytes, %#8jx in memory\n",
			    i, (uintmax_t)seg.offset, (uintmax_t)seg.filesz,
			    (uintmax_t)seg.memsz);
		else if (ioctl(dev, QCOM_PAS_LOAD, &seg) < 0)
			err(1, "%s: segment %d", name, i);
		free(data);
	}
	if (dry)
		return;
	if (ioctl(dev, QCOM_PAS_START, &timeout) < 0) {
		warn("%s: start", name);
		close(dev);
		status(name);
		exit(1);
	}
	close(dev);
	free(meta);
	munmap((void *)img, sb.st_size);
	close(fd);
	status(name);
}

static void
stop(const char *name)
{
	int fd;

	fd = open_dev(name);
	if (ioctl(fd, QCOM_PAS_STOP) < 0)
		err(1, "%s: stop", name);
	close(fd);
	status(name);
}

static void
usage(void)
{
	fprintf(stderr, "usage: pro1x-dsp start NAME [FIRMWARE]\n"
	    "       pro1x-dsp stop NAME\n"
	    "       pro1x-dsp status [NAME]\n"
	    "       pro1x-dsp -n start NAME FIRMWARE\n"
	    "NAME: adsp, cdsp or modem\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	char fw[1024];

	if (argc > 1 && strcmp(argv[1], "-n") == 0) {
		dry = 1;
		argc--;
		argv++;
	}
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "status") == 0 && argc == 2) {
		status("adsp");
		status("cdsp");
		status("modem");
	} else if (strcmp(argv[1], "status") == 0 && argc == 3)
		status(argv[2]);
	else if (strcmp(argv[1], "start") == 0 && (argc == 3 || argc == 4)) {
		if (argc == 4)
			snprintf(fw, sizeof(fw), "%s", argv[3]);
		else
			snprintf(fw, sizeof(fw), "%s/%s.mbn", FWDIR, argv[2]);
		start(argv[2], fw);
	} else if (strcmp(argv[1], "stop") == 0 && argc == 3)
		stop(argv[2]);
	else
		usage();
	return (0);
}
