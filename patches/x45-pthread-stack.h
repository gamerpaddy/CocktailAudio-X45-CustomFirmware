/* Give every thread a real stack under musl.
 *
 * musl's default thread stack is 128 KiB; glibc's is 8 MiB. Software written for
 * glibc (shairport-sync keeps sizeable buffers on the stack of its player and
 * decoder threads) overruns the guard page after a few seconds and dies with
 * SIGSEGV / SEGV_ACCERR. Compiler flags cannot change musl's default, so this
 * header is force-included (-include) into every translation unit and routes
 * pthread_create() through a wrapper that supplies a 1 MiB stack whenever the
 * caller did not ask for specific attributes.
 */
#ifndef X45_PTHREAD_STACK_H
#define X45_PTHREAD_STACK_H

#include <pthread.h>

#define X45_THREAD_STACK_BYTES (1024 * 1024)

static inline int x45_pthread_create(pthread_t *t, const pthread_attr_t *a,
				     void *(*fn)(void *), void *arg)
{
	pthread_attr_t own;
	int r;

	if (a)
		return pthread_create(t, a, fn, arg);
	pthread_attr_init(&own);
	pthread_attr_setstacksize(&own, X45_THREAD_STACK_BYTES);
	r = pthread_create(t, &own, fn, arg);
	pthread_attr_destroy(&own);
	return r;
}

/* defined after the wrapper so the wrapper itself calls the real function */
#define pthread_create x45_pthread_create

#endif /* X45_PTHREAD_STACK_H */
