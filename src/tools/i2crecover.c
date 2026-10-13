/* i2crecover - free the audio-board I2C bus (i2c-0, i.MX6 I2C1) when a slave holds SDA low.
 *
 * A transfer that was cut off in the middle of a byte (a zero-length read, a killed program) leaves the
 * addressed chip driving SDA low, waiting for clocks. From then on every access to the audio chips fails
 * ("i2cget: Read failed", the /proc/x45 register files empty, input switches refused). The cure is the standard one:
 * clock SCL by hand until the slave lets go of SDA.
 *
 * The pads (device tree i2c1grp): SDA = CSI0_DAT8 = GPIO5_IO26, SCL = CSI0_DAT9 = GPIO5_IO27, both ALT4.
 * This borrows the SCL pad as a GPIO for a few clock pulses and puts it back. It refuses to touch anything
 * unless the pad muxes read as the I2C function and SDA really is stuck low.
 *
 *   i2crecover          check; recover when SDA is stuck
 *   i2crecover -n       only report the line levels
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define IOMUXC      0x020E0000u
#define GPIO5       0x020AC000u
#define MUX_SDA     0x278u      /* SW_MUX_CTL_PAD_CSI0_DAT8 */
#define MUX_SCL     0x27Cu      /* SW_MUX_CTL_PAD_CSI0_DAT9 */
#define SDA_BIT     (1u << 26)
#define SCL_BIT     (1u << 27)
#define GDIR        1           /* word offsets in the GPIO block: DR, GDIR, PSR */
#define PSR         2

static volatile uint32_t *iomuxc, *gpio;

static int sda(void) { return !!(gpio[PSR] & SDA_BIT); }
static int scl(void) { return !!(gpio[PSR] & SCL_BIT); }
static void scl_low(void) { gpio[GDIR] |= SCL_BIT; }       /* DR is 0: drive low */
static void scl_release(void) { gpio[GDIR] &= ~SCL_BIT; }  /* the pull-up takes it high */

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC), i, check_only = argc > 1 && !strcmp(argv[1], "-n");
	uint32_t m_sda, m_scl, orig;

	if (fd < 0) {
		perror("/dev/mem");
		return 2;
	}
	iomuxc = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IOMUXC);
	gpio = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO5);
	if (iomuxc == MAP_FAILED || gpio == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	m_sda = iomuxc[MUX_SDA / 4];
	m_scl = iomuxc[MUX_SCL / 4];
	printf("pad mux: SDA=0x%x SCL=0x%x   lines: SDA=%d SCL=%d\n", m_sda, m_scl, sda(), scl());
	if ((m_sda & 7) != 4 || (m_scl & 7) != 4) {
		fprintf(stderr, "the pads are not muxed as I2C1 (ALT4): not touching them\n");
		return 3;
	}
	if (sda() && scl()) {
		printf("both lines are high: the bus is free\n");
		return 0;
	}
	if (check_only)
		return 1;
	if (!scl()) {
		fprintf(stderr, "SCL is held low as well: not a stuck slave, not touching it\n");
		return 3;
	}
	orig = m_scl;
	iomuxc[MUX_SCL / 4] = 5 | (orig & ~7u);      /* ALT5: GPIO5_IO27, SION as it was */
	gpio[0] &= ~SCL_BIT;
	for (i = 0; i < 18 && !sda(); i++) {         /* a byte and its acknowledge: nine clocks are enough, 18 is generous */
		scl_low();
		usleep(20);
		scl_release();
		usleep(20);
	}
	/* with SDA and SCL both high the bus is idle, which is all the controller needs for its next START */
	printf("after %d clock pulses: SDA=%d SCL=%d\n", i, sda(), scl());
	iomuxc[MUX_SCL / 4] = orig;                  /* the pad goes back to the I2C controller */
	usleep(100);
	printf("restored: SDA=%d SCL=%d\n", sda(), scl());
	return sda() && scl() ? 0 : 1;
}
