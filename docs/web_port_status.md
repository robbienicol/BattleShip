# Browser (WebAssembly) port — status

Goal: run BattleShip in the browser so rollback netplay (port/rollback/) can
power online matches on the ssb64-web site. Players supply their own ROM; no
extracted assets may ever be deployed.

## Build

```bash
source ../../emsdk/emsdk_env.sh          # emsdk lives in ~/Documents/ssb64-web/emsdk
embuilder build zlib sdl2
git -C libultraship apply ../patches/libultraship-emscripten.patch   # until libultraship is forked
emcmake cmake -S . -B build-web -GNinja -DSSB64_VERSION=us -DUSE_OPENGLES=ON \
  -DDISABLE_SCRIPTING=ON -DLUS_GCADAPTER=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-fwasm-exceptions -DCMAKE_CXX_FLAGS=-fwasm-exceptions
cmake --build build-web -j
```

Local test only: copy debug_tools/web/test.html into build-web (list the
build-web/assets files in it) and serve build-web (e.g. python -m http.server).
build-web holds assets extracted from your ROM — never publish it.

## Done

- Compiles and links (`BattleShip.wasm`, ~16 MB with assertions/profiling names).
- Engine changes (patches/libultraship-emscripten.patch): Emscripten
  dependency file (SDL2/zlib ports, others from source), hidapi stub, no GLEW,
  no resource-loader thread pool (tasks run inline), synchronous logger, no
  frame-pacing sleep.
- Port: Emscripten fiber coroutines (port/coroutine_emscripten.cpp, Asyncify),
  desktop-only features off (updater, Discord, shader downloader, window icon,
  watchdog/RenderDoc stubbed in port/web_debug_stubs.cpp), main loop yields
  to the browser with emscripten_sleep between frames.
- In the browser: boots, creates every N64 thread as a fiber, compiles all 74
  Fast3D shaders under WebGL2, enters the frame loop.

## Current blocker

The first frame never completes. What is known (2026-10-08):

- `-fwasm-exceptions` is incompatible with `-sASYNCIFY` (emcc warns; code
  mixing them miscompiles). The build now uses `-fexceptions` (JS-based EH).
- The fiber layer works on its own: a standalone test of
  port/coroutine_emscripten.cpp (nested resume/yield, indirect calls across
  yields) passes under node.
- coroutine_emscripten.cpp currently logs the first 200 switches ("FIB ...",
  temporary). Trace of frame 1: threads 8 and 5 resume and yield normally;
  thread 3 (scheduler) resumes, wakes from osRecvMesg with VRETRACE, and then
  execution simply stops — no yield, no return to the resumer, no console
  error, Asyncify idle (state 0, no pending sleep), runtime not aborted.
- Frame pacing sleeps are disabled on the web (gfx_sdl2 SyncFramerateWithTime,
  gameloop fallback pacing); the log flushes every line on the web.

Next steps:
1. Log each step of sySchedulerVRetrace (osSendMesg to each client,
   sySchedulerSwapBuffer, sySchedulerExecuteTasksAll) to find the call that
   never returns.
2. Check whether a C++ exception thrown inside a fiber is swallowed by the
   fiber trampoline with JS-based exceptions; wrap the fiber entry in
   try/catch + port_log.
3. If still unclear, build with -sASYNCIFY_DEBUG=1 for the first frame.
4. Then: measure Asyncify cost; restrict instrumentation if needed. Rollback
   on the web also needs a fiber-aware coroutine pool save/load.
