#define _GNU_SOURCE
#include "../libx45/persist.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/x45-persist-test-XXXXXX", cfg[256], queue[256], line[256];
    int i, j, status, count = 0, seen[8][200] = {{0}};
    pid_t kids[8];
    FILE *f;
    assert(mkdtemp(dir));
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(queue, sizeof queue, "%s/queue", dir);
    f = fopen(cfg, "w"); assert(f); fputs("# keep me\nuntouched = yes", f); fclose(f);
    for (i = 0; i < 8; i++) {
        kids[i] = fork(); assert(kids[i] >= 0);
        if (!kids[i]) {
            char key[32];
            snprintf(key, sizeof key, "writer%d", i);
            for (j = 0; j < 200; j++) {
                snprintf(line, sizeof line, "%d %d\n", i, j);
                assert(x45_queue_append(queue, line, strlen(line)) == 0);
                if (j % 20 == 0) assert(x45_config_set(cfg, key, "preserved") == 0);
            }
            _exit(0);
        }
    }
    while (count < 1600) {
        f = x45_queue_take(queue);
        if (!f) { usleep(1000); continue; }
        while (fgets(line, sizeof line, f)) {
            assert(sscanf(line, "%d %d", &i, &j) == 2);
            assert(i >= 0 && i < 8 && j >= 0 && j < 200);
            assert(!seen[i][j]++); count++;
        }
        fclose(f);
    }
    for (i = 0; i < 8; i++) { assert(waitpid(kids[i], &status, 0) == kids[i]); assert(status == 0); }
    assert(x45_config_set(cfg, "web_token", "secret") == 0);
    assert(x45_config_set(cfg, "web_token", "") == 0);
    assert(x45_config_set(cfg, "bad", "injected\nkey=value") == -1);
    f = fopen(cfg, "r"); assert(f); count = 0;
    while (fgets(line, sizeof line, f)) {
        assert(!strstr(line, "web_token"));
        if (strstr(line, "= preserved")) count++;
    }
    fclose(f); assert(count == 8);
    assert(x45_queue_append(queue, "x", 65537) == -1);
    /* the file lock: eight processes each add 1 to a counter 50 times, read-modify-write under x45_file_lock */
    {
        char counter[256];
        snprintf(counter, sizeof counter, "%s/counter", dir);
        f = fopen(counter, "w"); assert(f); fputs("0\n", f); fclose(f);
        for (i = 0; i < 8; i++) {
            kids[i] = fork(); assert(kids[i] >= 0);
            if (!kids[i]) {
                for (j = 0; j < 50; j++) {
                    int lk = x45_file_lock(counter), v = 0;
                    FILE *c = fopen(counter, "r"); assert(lk >= 0 && c);
                    assert(fscanf(c, "%d", &v) == 1); fclose(c);
                    usleep(200);   /* a window in which an unlocked writer would be overtaken */
                    c = fopen(counter, "w"); assert(c); fprintf(c, "%d\n", v + 1); fclose(c);
                    x45_file_unlock(lk);
                }
                _exit(0);
            }
        }
        for (i = 0; i < 8; i++) { assert(waitpid(kids[i], &status, 0) == kids[i]); assert(status == 0); }
        f = fopen(counter, "r"); assert(f); assert(fscanf(f, "%d", &count) == 1); fclose(f);
        assert(count == 400);
        unlink(counter);
        snprintf(line, sizeof line, "%s.lock", counter); unlink(line);
    }
    unlink(cfg); unlink(queue);
    snprintf(line, sizeof line, "%s.lock", cfg); unlink(line);
    snprintf(line, sizeof line, "%s.lock", queue); unlink(line);
    assert(rmdir(dir) == 0);
    puts("persist: 1600 concurrent commands delivered exactly once; all 8 settings preserved");
    return 0;
}
