/* nettp - tiny TCP throughput helper for checking a link from the PC side.
 *
 *   nettp serve PORT [MB]     accept connections forever: send MB megabytes (default 40) to every
 *                             client that connects, then close; then do one more round that only
 *                             reads (the client uploads) and reports what it received
 *   PC side: read the stream to the end (download), then write to the same port (upload).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 3 || strcmp(argv[1], "serve")) {
		fprintf(stderr, "usage: nettp serve PORT [MB]\n");
		return 2;
	}
	int port = atoi(argv[2]), mb = argc > 3 ? atoi(argv[3]) : 40;
	int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
	if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 2)) { perror("bind"); return 1; }
	static char buf[65536];
	for (;;) {
		int c = accept(s, NULL, NULL);
		if (c < 0) continue;
		double t0 = now();
		long sent = 0;
		for (long i = 0; i < (long)mb * 16; i++) {
			ssize_t n = write(c, buf, sizeof buf);
			if (n <= 0) break;
			sent += n;
		}
		shutdown(c, SHUT_WR);
		double t1 = now();
		long got = 0;
		ssize_t n;
		double t2 = 0;
		while ((n = read(c, buf, sizeof buf)) > 0) {
			if (!got) t2 = now();
			got += n;
		}
		double t3 = now();
		printf("sent %.1f MB in %.2fs = %.1f Mbit/s; received %.1f MB in %.2fs = %.1f Mbit/s\n",
		       sent / 1048576.0, t1 - t0, sent * 8 / 1e6 / (t1 - t0),
		       got / 1048576.0, got ? t3 - t2 : 0.0, got && t3 > t2 ? got * 8 / 1e6 / (t3 - t2) : 0.0);
		fflush(stdout);
		close(c);
	}
}
