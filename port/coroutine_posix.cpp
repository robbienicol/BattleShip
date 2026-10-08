/**
 * coroutine_posix.cpp — POSIX ucontext-based coroutine implementation.
 *
 * Each coroutine uses a ucontext_t with a separate stack.
 * swapcontext() provides the resume/yield mechanism.
 */

#if !defined(_WIN32) && !defined(__ANDROID__)

/*
 * macOS marks the ucontext / swapcontext routines as deprecated and hides
 * their declarations unless _XOPEN_SOURCE is defined before <ucontext.h>.
 * Define it here (not via the target's compile definitions) so the rest of
 * the port layer still sees strictly-POSIX symbols.
 *
 * Android's bionic libc dropped getcontext/makecontext/swapcontext entirely
 * (they were never supported on aarch64). The Android branch lives in
 * coroutine_android.cpp and uses a pthread-based fallback.
 */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
/* _XOPEN_SOURCE puts Darwin headers in strict-POSIX mode, which hides BSD
 * extensions like MAP_ANON(YMOUS); _DARWIN_C_SOURCE re-exposes them. */
#define _DARWIN_C_SOURCE 1
#endif

#include "coroutine.h"
#include "port_watchdog.h"

#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define MIN_STACK_SIZE 32768

struct PortCoroutine {
	ucontext_t ctx;
	ucontext_t caller_ctx;
	void (*entry)(void *);
	void *arg;
	int finished;
	char *stack_mem;   /* mmap base (guard page at the bottom) */
	size_t stack_total; /* full mapping length incl. guard page */
};

static thread_local PortCoroutine *sCurrentCoroutine = NULL;

static size_t port_page_size(void)
{
	static size_t ps = 0;
	if (ps == 0) {
		long v = sysconf(_SC_PAGESIZE);
		ps = (v > 0) ? (size_t)v : 4096;
	}
	return ps;
}

/* ========================================================================= */
/*  Internal: ucontext entry wrapper                                         */
/* ========================================================================= */

/*
 * makecontext requires a function with int parameters. A host pointer may be
 * wider than int, so on LP64/LLP64 (8-byte pointers) we split the
 * PortCoroutine pointer into a low and a high 32-bit half. On ILP32 (e.g.
 * 32-bit x86) a pointer already fits in a single int, and shifting a 4-byte
 * uintptr_t by 32 is undefined behavior — so the high half is unused there.
 */
static void ucontext_entry(unsigned int lo, unsigned int hi)
{
#if UINTPTR_MAX > 0xFFFFFFFFu
	uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
#else
	(void)hi;
	uintptr_t ptr = (uintptr_t)lo;
#endif
	PortCoroutine *co = (PortCoroutine *)ptr;

	co->entry(co->arg);

	co->finished = 1;
	sCurrentCoroutine = NULL;

	/* Return to caller via the linked context (set by swapcontext). */
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void port_coroutine_init_main(void)
{
	/* No-op on POSIX — ucontext doesn't require main thread conversion. */
}

PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg,
                                     size_t stack_size)
{
	PortCoroutine *co;
	uintptr_t ptr;

	if (stack_size < MIN_STACK_SIZE) {
		stack_size = MIN_STACK_SIZE;
	}

	co = (PortCoroutine *)calloc(1, sizeof(PortCoroutine));
	if (co == NULL) {
		return NULL;
	}

	/* mmap the stack with a PROT_NONE guard page at the low end so a
	 * coroutine stack overflow faults immediately (caught by the crash
	 * handler with a useful fault_addr) instead of silently corrupting
	 * adjacent heap allocations. Stacks grow down toward the guard. */
	size_t ps = port_page_size();
	stack_size = (stack_size + ps - 1) & ~(ps - 1);
	co->stack_total = stack_size + ps;
	void *mem = mmap(NULL, co->stack_total, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		free(co);
		return NULL;
	}
	mprotect(mem, ps, PROT_NONE);
	co->stack_mem = (char *)mem;

	co->entry = entry;
	co->arg = arg;
	co->finished = 0;

	if (getcontext(&co->ctx) == -1) {
		munmap(co->stack_mem, co->stack_total);
		free(co);
		return NULL;
	}

	co->ctx.uc_stack.ss_sp = co->stack_mem + ps;
	co->ctx.uc_stack.ss_size = stack_size;
	co->ctx.uc_link = &co->caller_ctx; /* return to caller when entry returns */

	ptr = (uintptr_t)co;
	makecontext(&co->ctx, (void (*)(void))ucontext_entry, 2,
	            (unsigned int)(ptr & 0xFFFFFFFF),
#if UINTPTR_MAX > 0xFFFFFFFFu
	            (unsigned int)(ptr >> 32));
#else
	            (unsigned int)0);
#endif

	return co;
}

void port_coroutine_destroy(PortCoroutine *co)
{
	if (co == NULL) {
		return;
	}
	if (co == sCurrentCoroutine) {
		fprintf(stderr, "SSB64: port_coroutine_destroy on current coroutine\n");
		abort();
	}
	if (co->stack_mem != NULL) {
		munmap(co->stack_mem, co->stack_total);
		co->stack_mem = NULL;
	}
	free(co);
}

void port_coroutine_resume(PortCoroutine *co)
{
	if (co == NULL || co->finished) {
		return;
	}

	/* Save the current coroutine so nested resumes restore correctly.
	 * Example: main resumes Thread5, Thread5 resumes a GObj coroutine.
	 * When the GObj coroutine yields, sCurrentCoroutine must be restored
	 * to Thread5 (not NULL) so Thread5 can still yield later. */
	PortCoroutine *prev = sCurrentCoroutine;

	sCurrentCoroutine = co;
	swapcontext(&co->caller_ctx, &co->ctx);

	/* Restore the previous coroutine context for the caller. */
	sCurrentCoroutine = prev;
}

void port_coroutine_yield(void)
{
	PortCoroutine *co = sCurrentCoroutine;
	if (co == NULL) {
		fprintf(stderr, "SSB64: port_coroutine_yield called outside coroutine\n");
		return;
	}

	port_watchdog_note_yield();

	sCurrentCoroutine = NULL;
	swapcontext(&co->ctx, &co->caller_ctx);
	/* Returns here when resumed. */
}

int port_coroutine_is_finished(PortCoroutine *co)
{
	if (co == NULL) {
		return 1;
	}
	return co->finished;
}

int port_coroutine_in_coroutine(void)
{
	return sCurrentCoroutine != NULL;
}

/* ========================================================================= */
/*  Pooled coroutines (rollback support)                                     */
/* ========================================================================= */

#define POOL_SLOTS 32
#define POOL_MAGIC 0x43504F4Fu /* 'CPOO' */

struct PoolSlot {
	PortCoroutine co;
	int in_use;
};

static PoolSlot sPool[POOL_SLOTS];

static int pool_index(PortCoroutine *co)
{
	if (co < &sPool[0].co || co > &sPool[POOL_SLOTS - 1].co) {
		return -1;
	}
	size_t offset = (size_t)((char *)co - (char *)&sPool[0]);
	if (offset % sizeof(PoolSlot) != 0) {
		return -1;
	}
	return (int)(offset / sizeof(PoolSlot));
}

/* Saved stack pointer of a suspended coroutine. */
static uintptr_t pool_saved_sp(PortCoroutine *co)
{
#if defined(__APPLE__) && defined(__aarch64__)
	return (uintptr_t)co->ctx.uc_mcontext->__ss.__sp;
#elif defined(__APPLE__) && defined(__x86_64__)
	return (uintptr_t)co->ctx.uc_mcontext->__ss.__rsp;
#elif defined(__linux__) && defined(__aarch64__)
	return (uintptr_t)co->ctx.uc_mcontext.sp;
#elif defined(__linux__) && defined(__x86_64__)
	return (uintptr_t)co->ctx.uc_mcontext.gregs[REG_RSP];
#else
	return 0;
#endif
}

PortCoroutine *port_coroutine_pool_acquire(void (*entry)(void *), void *arg, size_t stack_size)
{
	if (stack_size < MIN_STACK_SIZE) {
		stack_size = MIN_STACK_SIZE;
	}
	for (int i = 0; i < POOL_SLOTS; i++) {
		PoolSlot *slot = &sPool[i];
		if (slot->in_use) {
			continue;
		}
		PortCoroutine *co = &slot->co;
		size_t ps = port_page_size();
		size_t want = ((stack_size + ps - 1) & ~(ps - 1)) + ps;
		if (co->stack_mem == NULL || co->stack_total < want) {
			if (co->stack_mem != NULL) {
				munmap(co->stack_mem, co->stack_total);
			}
			void *mem = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (mem == MAP_FAILED) {
				co->stack_mem = NULL;
				return NULL;
			}
			mprotect(mem, ps, PROT_NONE);
			co->stack_mem = (char *)mem;
			co->stack_total = want;
		}
		co->entry = entry;
		co->arg = arg;
		co->finished = 0;
		if (getcontext(&co->ctx) == -1) {
			return NULL;
		}
		co->ctx.uc_stack.ss_sp = co->stack_mem + ps;
		co->ctx.uc_stack.ss_size = co->stack_total - ps;
		co->ctx.uc_link = &co->caller_ctx;
		uintptr_t ptr = (uintptr_t)co;
		makecontext(&co->ctx, (void (*)(void))ucontext_entry, 2,
		            (unsigned int)(ptr & 0xFFFFFFFF),
#if UINTPTR_MAX > 0xFFFFFFFFu
		            (unsigned int)(ptr >> 32));
#else
		            (unsigned int)0);
#endif
		slot->in_use = 1;
		return co;
	}
	fprintf(stderr, "SSB64: coroutine pool exhausted (%d slots)\n", POOL_SLOTS);
	return NULL;
}

void port_coroutine_pool_release(PortCoroutine *co)
{
	int i = pool_index(co);
	if (i < 0) {
		port_coroutine_destroy(co);
		return;
	}
	if (co == sCurrentCoroutine) {
		fprintf(stderr, "SSB64: port_coroutine_pool_release on current coroutine\n");
		abort();
	}
	sPool[i].in_use = 0;
}

int port_coroutine_is_pooled(PortCoroutine *co)
{
	return pool_index(co) >= 0;
}

/* Layout: magic, live count, then per live slot: index, struct bytes,
 * stack offset, stack length, stack bytes (from the saved SP to the top). */
size_t port_coroutine_pool_save(unsigned char *buf, size_t cap)
{
	size_t at = 0;
	auto put = [&](const void *src, size_t len) -> bool {
		if (at + len > cap) return false;
		memcpy(buf + at, src, len);
		at += len;
		return true;
	};
	uint32_t magic = POOL_MAGIC, live = 0;
	for (int i = 0; i < POOL_SLOTS; i++) live += sPool[i].in_use ? 1 : 0;
	if (!put(&magic, sizeof(magic)) || !put(&live, sizeof(live))) return 0;

	for (int i = 0; i < POOL_SLOTS; i++) {
		if (!sPool[i].in_use) continue;
		PortCoroutine *co = &sPool[i].co;
		uint32_t index = (uint32_t)i;
		char *stack_lo = co->stack_mem + port_page_size();
		char *stack_hi = co->stack_mem + co->stack_total;
		/* Copy from just below the saved SP (red zone) to the top; if the SP
		 * is unknown or out of range, copy the whole stack. */
		uintptr_t sp = pool_saved_sp(co);
		char *from = stack_lo;
		if (sp > (uintptr_t)stack_lo + 256 && sp <= (uintptr_t)stack_hi) {
			from = (char *)(sp - 256);
		}
		uint64_t off = (uint64_t)(from - co->stack_mem);
		uint64_t len = (uint64_t)(stack_hi - from);
		if (!put(&index, sizeof(index)) || !put(co, sizeof(*co)) || !put(&off, sizeof(off)) ||
		    !put(&len, sizeof(len)) || !put(from, (size_t)len)) {
			return 0;
		}
	}
	return at;
}

int port_coroutine_pool_load(const unsigned char *buf, size_t len)
{
	size_t at = 0;
	auto get = [&](void *dst, size_t n) -> bool {
		if (at + n > len) return false;
		memcpy(dst, buf + at, n);
		at += n;
		return true;
	};
	uint32_t magic = 0, live = 0;
	if (!get(&magic, sizeof(magic)) || magic != POOL_MAGIC || !get(&live, sizeof(live))) return 0;

	for (int i = 0; i < POOL_SLOTS; i++) sPool[i].in_use = 0;
	for (uint32_t n = 0; n < live; n++) {
		uint32_t index = 0;
		uint64_t off = 0, stack_len = 0;
		if (!get(&index, sizeof(index)) || index >= POOL_SLOTS) return 0;
		PortCoroutine *co = &sPool[index].co;
		/* The slot's stack mapping is never freed, so the saved struct (which
		 * points at it) and the stack bytes go back to the same addresses. */
		if (!get(co, sizeof(*co)) || !get(&off, sizeof(off)) || !get(&stack_len, sizeof(stack_len))) return 0;
		if (co->stack_mem == NULL || off + stack_len > co->stack_total) return 0;
		if (!get(co->stack_mem + off, (size_t)stack_len)) return 0;
		sPool[index].in_use = 1;
	}
	return 1;
}

#endif /* !_WIN32 && !__ANDROID__ */
