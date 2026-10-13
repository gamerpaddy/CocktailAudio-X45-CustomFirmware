/* libx45 master-clock family - see x45.h.
 *
 * Kept in its own file because it needs no /dev/x45 handle and nothing else
 * from x45.c, so it can be linked alone into programs that only have to pick
 * the right clock before they open a PCM (MPD, via libx45clock.a). */
#define _GNU_SOURCE
#include "x45.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define X45_GPIO_PROC "/proc/x45/gpio"

int x45_clock_family(unsigned rate)
{
	/* 11025 and 8000 are the greatest common bases of each family: every
	 * standard rate is a power-of-two multiple of one of them. 44.1k-family
	 * rates (11025 22050 44100 88200 176400 352800 ...) are multiples of
	 * 11025; the 48k family (8000 12000 16000 24000 32000 48000 96000 ...)
	 * are multiples of 8000 or 12000. */
	if (rate == 0)
		return -1;
	if (rate % 11025 == 0)
		return X45_CLOCK_44;
	if (rate % 8000 == 0 || rate % 12000 == 0)
		return X45_CLOCK_48;
	return -1;
}

int x45_clock_get(void)
{
	char buf[4096], *p;   /* the whole listing: the line may come late in it */
	ssize_t n;
	int fd = open(X45_GPIO_PROC, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return n < 0 ? -errno : -EIO;
	buf[n] = '\0';
	p = strstr(buf, "FS_4844");
	if (!p || !(p = strchr(p, ':')))
		return -ENOENT;
	return p[1] == ' ' ? (p[2] == '1') : (p[1] == '1');
}

int x45_clock_select(unsigned rate)
{
	char msg[32];
	ssize_t n;
	int fam = x45_clock_family(rate), fd, want;

	if (fam < 0)
		return -EINVAL;
	want = fam;
	/* skip the write if it is already right: every write makes the driver log
	 * a line, and a redundant toggle risks a glitch on a settled clock */
	if (x45_clock_get() == want)
		return 0;
	fd = open(X45_GPIO_PROC, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = snprintf(msg, sizeof(msg), "FS_4844:%d", want);
	n = write(fd, msg, (size_t)n);
	close(fd);
	if (n < 0)
		return -errno;
	return x45_clock_get() == want ? 0 : -EIO;
}

