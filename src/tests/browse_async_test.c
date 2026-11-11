#define main x45ui_program_main
#include "../x45ui/main.c"
#undef main
#include <assert.h>

int main(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t alen = sizeof a;
    struct ui *u = calloc(1, sizeof *u);
    int ls = socket(AF_INET, SOCK_STREAM, 0), status;
    pid_t server;
    long start;
    assert(u && ls >= 0);
    assert(bind(ls, (void *)&a, sizeof a) == 0 && listen(ls, 2) == 0);
    assert(getsockname(ls, (void *)&a, &alen) == 0);
    server = fork(); assert(server >= 0);
    if (!server) {
        int c = accept(ls, NULL, NULL);
        write(c, "OK MPD 0.23\n", 12);
        sleep(3); close(c); /* first listing is deliberately slow */
        c = accept(ls, NULL, NULL);
        write(c, "OK MPD 0.23\n", 12);
        { char b[256]; read(c, b, sizeof b); }
        { const char *answer = "file: latest/song.flac\nTitle: Latest\nOK\n"; write(c, answer, strlen(answer)); }
        close(c); close(ls); _exit(0);
    }
    close(ls);
    u->mpd_port = ntohs(a.sin_port);
    start = now_ms();
    browse_load(u, "slow", NULL);
    assert(ms_age(now_ms(), start) < 100);
    usleep(100000);
    browse_load(u, "latest", NULL); /* stale result must not overwrite latest */
    while ((browse_job.active || browse_job.pending) && ms_age(now_ms(), start) < 7000) {
        browse_job_poll(u); usleep(10000);
    }
    assert(!browse_job.active && !browse_job.pending);
    assert(!strcmp(u->dir, "latest") && u->nrows == 1);
    assert(!strcmp(u->rows[0].path, "latest/song.flac"));
    assert(!u->dir_err[0]);
    waitpid(server, &status, 0); assert(status == 0);
    free(u->rows); free(u);
    puts("browse: returns under 100ms with stalled MPD; stale result discarded; latest listing adopted");
    return 0;
}
