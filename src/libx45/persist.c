#define _GNU_SOURCE
#include "persist.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int lock_path(const char *path, int nonblock)
{
    char name[PATH_MAX];
    int fd;
    if (snprintf(name, sizeof name, "%s.lock", path) >= (int)sizeof name) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(name, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    while (flock(fd, LOCK_EX | (nonblock ? LOCK_NB : 0)) < 0) {
        if (errno == EINTR) continue;
        close(fd);
        return -1;
    }
    return fd;
}

int x45_file_lock(const char *path)
{
    return lock_path(path, 0);
}

void x45_file_unlock(int lock)
{
    if (lock >= 0) close(lock);
}

int x45_config_set(const char *path, const char *key, const char *value)
{
    char temp[PATH_MAX], parent[PATH_MAX], *line = NULL;
    size_t cap = 0, kl = strlen(key);
    ssize_t n;
    int lock, fd = -1, ok = 0, last = '\n';
    FILE *in = NULL, *out = NULL;
    struct stat st;
    if (!kl || strpbrk(key, "= \t\r\n") || strpbrk(value, "\r\n")) return -1;
    if (snprintf(temp, sizeof temp, "%s.tmp.XXXXXX", path) >= (int)sizeof temp) return -1;
    lock = lock_path(path, 0);
    if (lock < 0) return -1;
    in = fopen(path, "r");
    if (!in && errno != ENOENT) goto done;
    fd = mkstemp(temp);
    if (fd < 0) goto done;
    if (in && fstat(fileno(in), &st) != 0) goto done;
    if (fchmod(fd, in ? st.st_mode & 0777 : 0600) != 0) goto done;
    out = fdopen(fd, "w");
    if (!out) goto done;
    fd = -1;
    while (in && (n = getline(&line, &cap, in)) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, kl)) {
            p += kl;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') continue;
        }
        if (fwrite(line, 1, (size_t)n, out) != (size_t)n) goto done;
        if (n) last = line[n - 1];
    }
    if (in && ferror(in)) goto done;
    if (value[0]) {
        if (last != '\n' && fputc('\n', out) == EOF) goto done;
        if (fprintf(out, "%s = %s\n", key, value) < 0) goto done;
    }
    if (fflush(out) || fsync(fileno(out))) goto done;
    { int r = fclose(out); out = NULL; if (r) goto done; }
    if (rename(temp, path)) goto done;
    snprintf(parent, sizeof parent, "%s", path);
    { char *slash = strrchr(parent, '/');
      if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
      else strcpy(parent, "."); }
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || fsync(fd)) goto done;
    ok = 1;
done:
    free(line);
    if (in) fclose(in);
    if (out) fclose(out);
    if (fd >= 0) close(fd);
    if (!ok) unlink(temp);
    close(lock);
    return ok ? 0 : -1;
}

int x45_queue_append(const char *path, const void *data, size_t len)
{
    int lock = lock_path(path, 0), fd = -1, ok = 0;
    struct stat st;
    size_t off = 0;
    if (lock < 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0 || fstat(fd, &st)) goto done;
    if (len > 65536 || st.st_size > (off_t)(65536 - len)) { errno = ENOSPC; goto done; }
    while (off < len) {
        ssize_t n = write(fd, (const char *)data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ftruncate(fd, st.st_size); goto done; }
        off += (size_t)n;
    }
    ok = 1;
done:
    if (fd >= 0) close(fd);
    close(lock);
    return ok ? 0 : -1;
}

FILE *x45_queue_take(const char *path)
{
    int lock = lock_path(path, 1);
    FILE *f;
    if (lock < 0) return NULL;
    f = fopen(path, "r");
    if (f && unlink(path)) { fclose(f); f = NULL; }
    close(lock);
    return f;
}
