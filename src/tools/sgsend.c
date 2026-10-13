/* sgsend <sg device> <cdb hex> [in <len>]: send one raw SCSI CDB with SG_IO, print sense/data */
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned char cdb[16] = { 0 }, data[512] = { 0 }, sense[32] = { 0 };
	struct sg_io_hdr h;
	size_t n, i, dlen = 0;
	int fd, r;

	if (argc < 3)
		return fprintf(stderr, "usage: sgsend /dev/sgN cdbhex [in len]\n"), 2;
	n = strlen(argv[2]) / 2;
	if (n > 16)
		n = 16;
	for (i = 0; i < n; i++) {
		unsigned v;

		sscanf(argv[2] + 2 * i, "%2x", &v);
		cdb[i] = (unsigned char)v;
	}
	if (argc >= 5 && !strcmp(argv[3], "in"))
		dlen = (size_t)atoi(argv[4]);
	fd = open(argv[1], O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return perror("open"), 1;
	memset(&h, 0, sizeof h);
	h.interface_id = 'S';
	h.cmd_len = (unsigned char)(n > 12 ? 16 : n);
	h.cmdp = cdb;
	h.dxfer_direction = dlen ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
	h.dxfer_len = (unsigned)dlen;
	h.dxferp = data;
	h.mx_sb_len = sizeof sense;
	h.sbp = sense;
	h.timeout = 5000;
	r = ioctl(fd, SG_IO, &h);
	printf("ioctl=%d status=%u host=%u driver=%u resid=%d\n", r, h.status, h.host_status, h.driver_status, h.resid);
	if (h.sb_len_wr) {
		printf("sense:");
		for (i = 0; i < h.sb_len_wr; i++)
			printf(" %02x", sense[i]);
		printf("\n");
	}
	if (dlen) {
		printf("data:");
		for (i = 0; i < dlen; i++)
			printf(" %02x", data[i]);
		printf("\n");
	}
	return 0;
}
