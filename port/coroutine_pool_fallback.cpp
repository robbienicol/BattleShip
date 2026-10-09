/**
 * coroutine_pool_fallback.cpp — pooled-coroutine API for backends that do not
 * support snapshotting yet (Win32 fibers, Android asm). Pooled coroutines are
 * plain coroutines and saving reports "unsupported", so rollback stays off.
 */
#if defined(_WIN32) || defined(__ANDROID__)

#include "coroutine.h"

PortCoroutine *port_coroutine_pool_acquire(void (*entry)(void *), void *arg, size_t stack_size)
{
	return port_coroutine_create(entry, arg, stack_size);
}

void port_coroutine_pool_release(PortCoroutine *co)
{
	port_coroutine_destroy(co);
}

int port_coroutine_is_pooled(PortCoroutine *co)
{
	(void)co;
	return 0;
}

size_t port_coroutine_pool_save(unsigned char *buf, size_t cap)
{
	(void)buf;
	(void)cap;
	return 0;
}

int port_coroutine_pool_load(const unsigned char *buf, size_t len)
{
	(void)buf;
	(void)len;
	return 0;
}

#endif
