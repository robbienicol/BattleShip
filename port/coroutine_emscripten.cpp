/**
 * coroutine_emscripten.cpp — coroutines for the browser build.
 *
 * Browsers have no ucontext; Emscripten fibers (built on Asyncify) provide
 * the same cooperative switch: each coroutine gets its own C stack plus an
 * Asyncify buffer that holds the unwound call stack while it is suspended.
 */
#if defined(__EMSCRIPTEN__)

#include "coroutine.h"
#include "port_watchdog.h"

#include <emscripten/fiber.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <exception>

extern "C" void port_log(const char *fmt, ...);

namespace {

constexpr size_t kMinStack = 64 * 1024;
constexpr size_t kMainAsyncifyStack = 256 * 1024;

emscripten_fiber_t sMainFiber;
char *sMainAsyncify = nullptr;

} /* namespace */

struct PortCoroutine {
	emscripten_fiber_t fiber;
	emscripten_fiber_t *caller; /* fiber that last resumed this one */
	void (*entry)(void *);
	void *arg;
	int finished;
	char *c_stack;
	char *asyncify_stack;
};

static PortCoroutine *sCurrent = nullptr;

/* A fiber entry must never return; park the finished coroutine instead. */
static void fiber_entry(void *p)
{
	PortCoroutine *co = static_cast<PortCoroutine *>(p);
	try {
		co->entry(co->arg);
	} catch (const std::exception &e) {
		port_log("FIB %p: uncaught exception: %s\n", (void *)co, e.what());
	} catch (...) {
		port_log("FIB %p: uncaught non-std exception\n", (void *)co);
	}
	co->finished = 1;
	for (;;) {
		emscripten_fiber_swap(&co->fiber, co->caller);
	}
}

void port_coroutine_init_main(void)
{
	if (sMainAsyncify == nullptr) {
		sMainAsyncify = static_cast<char *>(std::malloc(kMainAsyncifyStack));
		emscripten_fiber_init_from_current_context(&sMainFiber, sMainAsyncify, kMainAsyncifyStack);
	}
}

PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg, size_t stack_size)
{
	port_coroutine_init_main();
	if (stack_size < kMinStack) {
		stack_size = kMinStack;
	}
	PortCoroutine *co = static_cast<PortCoroutine *>(std::calloc(1, sizeof(PortCoroutine)));
	if (co == nullptr) {
		return nullptr;
	}
	co->entry = entry;
	co->arg = arg;
	co->c_stack = static_cast<char *>(std::aligned_alloc(16, stack_size));
	co->asyncify_stack = static_cast<char *>(std::malloc(stack_size));
	if (co->c_stack == nullptr || co->asyncify_stack == nullptr) {
		std::free(co->c_stack);
		std::free(co->asyncify_stack);
		std::free(co);
		return nullptr;
	}
	emscripten_fiber_init(&co->fiber, fiber_entry, co, co->c_stack, stack_size, co->asyncify_stack, stack_size);
	return co;
}

void port_coroutine_destroy(PortCoroutine *co)
{
	if (co == nullptr) {
		return;
	}
	if (co == sCurrent) {
		std::fprintf(stderr, "SSB64: port_coroutine_destroy on current coroutine\n");
		std::abort();
	}
	std::free(co->c_stack);
	std::free(co->asyncify_stack);
	std::free(co);
}

void port_coroutine_resume(PortCoroutine *co)
{
	if (co == nullptr || co->finished) {
		return;
	}
	PortCoroutine *prev = sCurrent;
	emscripten_fiber_t *from = (prev != nullptr) ? &prev->fiber : &sMainFiber;
	co->caller = from;
	sCurrent = co;
	emscripten_fiber_swap(from, &co->fiber);
	sCurrent = prev;
}

void port_coroutine_yield(void)
{
	PortCoroutine *co = sCurrent;
	if (co == nullptr) {
		std::fprintf(stderr, "SSB64: port_coroutine_yield called outside coroutine\n");
		return;
	}
	port_watchdog_note_yield();
	emscripten_fiber_swap(&co->fiber, co->caller);
}

int port_coroutine_is_finished(PortCoroutine *co)
{
	return (co == nullptr) ? 1 : co->finished;
}

int port_coroutine_in_coroutine(void)
{
	return sCurrent != nullptr;
}

/* ========================================================================= */
/*  Pooled coroutines (rollback support)                                     */
/* ========================================================================= */

/* Short-lived coroutines (GObj thread processes) come from fixed slots whose
 * C stack and Asyncify buffer are never freed. A suspended fiber is fully
 * described by its struct plus the used part of both buffers, all in linear
 * memory, so it can be saved and later put back at the same addresses. */

#define POOL_SLOTS 32
#define POOL_MAGIC 0x43504F57u /* 'CPOW' */

namespace {

struct PoolSlot {
	PortCoroutine co;
	size_t stack_size;
	int in_use;
};

PoolSlot sPool[POOL_SLOTS];

int pool_index(PortCoroutine *co)
{
	for (int i = 0; i < POOL_SLOTS; i++) {
		if (&sPool[i].co == co) return i;
	}
	return -1;
}

} /* namespace */

PortCoroutine *port_coroutine_pool_acquire(void (*entry)(void *), void *arg, size_t stack_size)
{
	port_coroutine_init_main();
	if (stack_size < kMinStack) {
		stack_size = kMinStack;
	}
	for (int i = 0; i < POOL_SLOTS; i++) {
		PoolSlot *slot = &sPool[i];
		if (slot->in_use) continue;
		PortCoroutine *co = &slot->co;
		if (co->c_stack == nullptr || slot->stack_size < stack_size) {
			std::free(co->c_stack);
			std::free(co->asyncify_stack);
			co->c_stack = static_cast<char *>(std::aligned_alloc(16, stack_size));
			co->asyncify_stack = static_cast<char *>(std::malloc(stack_size));
			if (co->c_stack == nullptr || co->asyncify_stack == nullptr) return nullptr;
			slot->stack_size = stack_size;
		}
		co->entry = entry;
		co->arg = arg;
		co->finished = 0;
		co->caller = nullptr;
		emscripten_fiber_init(&co->fiber, fiber_entry, co, co->c_stack, slot->stack_size, co->asyncify_stack,
		                      slot->stack_size);
		slot->in_use = 1;
		return co;
	}
	std::fprintf(stderr, "SSB64: coroutine pool exhausted (%d slots)\n", POOL_SLOTS);
	return nullptr;
}

void port_coroutine_pool_release(PortCoroutine *co)
{
	int i = pool_index(co);
	if (i < 0) {
		port_coroutine_destroy(co);
		return;
	}
	sPool[i].in_use = 0;
}

int port_coroutine_is_pooled(PortCoroutine *co)
{
	return pool_index(co) >= 0;
}

/* Layout: magic, live count, then per live slot: index, struct bytes, C stack
 * span (offset, length, bytes), Asyncify span (length, bytes). */
size_t port_coroutine_pool_save(unsigned char *buf, size_t cap)
{
	size_t at = 0;
	auto put = [&](const void *src, size_t len) -> bool {
		if (at + len > cap) return false;
		std::memcpy(buf + at, src, len);
		at += len;
		return true;
	};
	uint32_t magic = POOL_MAGIC, live = 0;
	for (int i = 0; i < POOL_SLOTS; i++) live += sPool[i].in_use ? 1 : 0;
	if (!put(&magic, sizeof(magic)) || !put(&live, sizeof(live))) return 0;

	for (int i = 0; i < POOL_SLOTS; i++) {
		if (!sPool[i].in_use) continue;
		PortCoroutine *co = &sPool[i].co;
		char *stack_lo = co->c_stack;
		char *stack_hi = co->c_stack + sPool[i].stack_size;
		char *sp = static_cast<char *>(co->fiber.stack_ptr);
		if (sp < stack_lo || sp > stack_hi) sp = stack_lo; /* unknown: copy all */
		uint32_t index = (uint32_t)i;
		uint32_t stack_off = (uint32_t)(sp - stack_lo);
		uint32_t stack_len = (uint32_t)(stack_hi - sp);
		char *async_end = static_cast<char *>(co->fiber.asyncify_data.stack_ptr);
		if (async_end < co->asyncify_stack || async_end > co->asyncify_stack + sPool[i].stack_size) {
			async_end = co->asyncify_stack + sPool[i].stack_size;
		}
		uint32_t async_len = (uint32_t)(async_end - co->asyncify_stack);
		if (!put(&index, sizeof(index)) || !put(co, sizeof(*co)) || !put(&stack_off, sizeof(stack_off)) ||
		    !put(&stack_len, sizeof(stack_len)) || !put(sp, stack_len) || !put(&async_len, sizeof(async_len)) ||
		    !put(co->asyncify_stack, async_len)) {
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
		std::memcpy(dst, buf + at, n);
		at += n;
		return true;
	};
	uint32_t magic = 0, live = 0;
	if (!get(&magic, sizeof(magic)) || magic != POOL_MAGIC || !get(&live, sizeof(live))) return 0;

	for (int i = 0; i < POOL_SLOTS; i++) sPool[i].in_use = 0;
	for (uint32_t n = 0; n < live; n++) {
		uint32_t index = 0, stack_off = 0, stack_len = 0, async_len = 0;
		if (!get(&index, sizeof(index)) || index >= POOL_SLOTS) return 0;
		PoolSlot *slot = &sPool[index];
		/* The slot's buffers are never freed, so the saved struct (which
		 * points at them) and the buffer bytes go back to the same place. */
		if (!get(&slot->co, sizeof(slot->co)) || !get(&stack_off, sizeof(stack_off)) ||
		    !get(&stack_len, sizeof(stack_len))) {
			return 0;
		}
		if (slot->co.c_stack == nullptr || stack_off + stack_len > slot->stack_size) return 0;
		if (!get(slot->co.c_stack + stack_off, stack_len) || !get(&async_len, sizeof(async_len))) return 0;
		if (async_len > slot->stack_size || !get(slot->co.asyncify_stack, async_len)) return 0;
		slot->in_use = 1;
	}
	return 1;
}

#endif /* __EMSCRIPTEN__ */
