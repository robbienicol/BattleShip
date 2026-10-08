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
	co->entry(co->arg);
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

#endif /* __EMSCRIPTEN__ */
