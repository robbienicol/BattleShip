/**
 * web_debug_stubs.cpp — browser (Emscripten) stand-ins for desktop-only
 * debugging tools: the hang watchdog (needs native backtraces and a thread)
 * and RenderDoc frame capture. Both become no-ops in the browser build.
 */
#if defined(__EMSCRIPTEN__)

#include "port_watchdog.h"
#include "renderdoc_trigger.h"

void port_watchdog_init(void) {}
void port_watchdog_shutdown(void) {}
void port_watchdog_note_yield(void) {}
void port_watchdog_note_resume_start(int thread_id) { (void)thread_id; }
void port_watchdog_note_resume_end(int thread_id) { (void)thread_id; }
void port_watchdog_note_frame_end(void) {}
void port_dump_backtrace(void) {}

void portRenderDocInit(void) {}
void portRenderDocOnFrame(unsigned int frame_count) { (void)frame_count; }
void portRenderDocShutdown(void) {}

#endif
