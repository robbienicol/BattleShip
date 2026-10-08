#pragma once

/**
 * coroutine.h — Platform-agnostic coroutine API for the SSB64 port.
 *
 * Provides cooperative multitasking to emulate N64 OS threads on PC.
 * Each N64 thread (scheduler, audio, controller, game logic) runs as
 * a coroutine that yields at blocking points (osRecvMesg with OS_MESG_BLOCK).
 * GObj thread processes also use coroutines for their yield/resume cycle.
 *
 * Platform backends:
 *   - Windows: Win32 Fibers (CreateFiber / SwitchToFiber)
 *   - POSIX:   ucontext_t  (makecontext / swapcontext)
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortCoroutine PortCoroutine;

/**
 * Initialize the main thread for coroutine support.
 * On Windows, this converts the calling thread to a fiber.
 * Must be called once before any other coroutine functions.
 */
void port_coroutine_init_main(void);

/**
 * Create a new coroutine.
 *
 * @param entry      Entry point function. Called with @p arg.
 *                   When entry returns, the coroutine is marked finished
 *                   and control returns to whoever last resumed it.
 * @param arg        Argument passed to entry.
 * @param stack_size Stack size in bytes. Minimum 16384 (16 KB).
 * @return           Opaque coroutine handle, or NULL on failure.
 */
PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg, size_t stack_size);

/**
 * Destroy a coroutine and free its resources.
 * Must not be called on a currently executing coroutine.
 */
void port_coroutine_destroy(PortCoroutine *co);

/**
 * Resume a coroutine from the main thread (or from another coroutine).
 * Execution transfers to the coroutine until it calls port_coroutine_yield()
 * or its entry function returns. Then control returns here.
 */
void port_coroutine_resume(PortCoroutine *co);

/**
 * Yield from the currently executing coroutine back to whoever resumed it.
 * Must only be called from within a coroutine (not the main thread).
 */
void port_coroutine_yield(void);

/**
 * Returns non-zero if the coroutine's entry function has returned.
 */
int port_coroutine_is_finished(PortCoroutine *co);

/**
 * Returns non-zero if the calling code is currently inside a coroutine
 * (as opposed to the main thread).
 */
int port_coroutine_in_coroutine(void);

/* ---- Pooled coroutines (rollback support) ----
 *
 * Short-lived coroutines (GObj thread processes such as the VS countdown)
 * come from a fixed pool whose stacks are never freed, so their full state
 * — saved registers plus stack contents — can be captured in a rollback
 * snapshot and put back later, even if the coroutine was destroyed or a
 * different one occupies the slot in between.
 */

/* Like port_coroutine_create, but from the pool. NULL if the pool is full. */
PortCoroutine *port_coroutine_pool_acquire(void (*entry)(void *), void *arg, size_t stack_size);

/* Returns a pooled coroutine to the pool (instead of port_coroutine_destroy). */
void port_coroutine_pool_release(PortCoroutine *co);

/* Non-zero if co came from the pool. */
int port_coroutine_is_pooled(PortCoroutine *co);

/* Serializes every live pooled coroutine. Returns bytes written, or 0 if
 * cap is too small or pooling is unsupported on this platform. */
size_t port_coroutine_pool_save(unsigned char *buf, size_t cap);

/* Restores the pool to a state written by port_coroutine_pool_save.
 * Must be called from the main thread (no coroutine running). */
int port_coroutine_pool_load(const unsigned char *buf, size_t len);

#ifdef __cplusplus
}
#endif
