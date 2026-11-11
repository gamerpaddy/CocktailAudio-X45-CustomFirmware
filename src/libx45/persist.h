#ifndef X45_PERSIST_H
#define X45_PERSIST_H
#include <stddef.h>
#include <stdio.h>
/* Locks use a separate, stable inode, never the file being renamed. */
int x45_config_set(const char *path, const char *key, const char *value);
int x45_queue_append(const char *path, const void *data, size_t len);
/* An exclusive lock for a read-modify-write of one file (the station list has three writers: the panel, the web
 * page and the router sync). The lock lives on PATH.lock, a stable inode of its own (scripts take the same lock with
 * `flock` on that file). Blocks until it is free; returns a descriptor for x45_file_unlock, or -1 (the caller then
 * goes on without: a lock that cannot be made must not stop the user from saving). */
int x45_file_lock(const char *path);
void x45_file_unlock(int lock);
/* Nonblocking: NULL means no work or another writer currently owns the lock. */
FILE *x45_queue_take(const char *path);
#endif
