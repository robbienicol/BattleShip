/* Force-included into every decomp game source (see CMakeLists.txt).
 *
 * Rollback netcode snapshots the simulation by copying memory. Placing every
 * global and static of the decomp game code into dedicated sections turns
 * "all game variables" into contiguous ranges the snapshot code can find by
 * their section bounds (port/rollback/rollback_state.cpp).
 *
 * Engine plumbing (threads, scheduler, render/audio pipeline, DMA, debug,
 * network transport, replay tooling) is compiled with SSB64_ROLLBACK_NOSNAP
 * and goes into a separate section that is never restored; the few gameplay
 * values living in those files are snapshotted individually. */
#if defined(__APPLE__)
#if defined(SSB64_ROLLBACK_NOSNAP)
#pragma clang section bss = "__DATA,__ssbnsbss" data = "__DATA,__ssbnsdata"
#else
#pragma clang section bss = "__DATA,__ssbbss" data = "__DATA,__ssbdata"
#endif
#elif defined(__EMSCRIPTEN__)
/* wasm-ld keeps custom-named data segments together in link order but emits
 * no start/stop symbols; port/rollback/markers/ supplies the bounds. Engine
 * plumbing simply stays in the normal .data/.bss. */
#if !defined(SSB64_ROLLBACK_NOSNAP)
#pragma clang section bss = "ssbbss" data = "ssbdata"
#endif
#endif
