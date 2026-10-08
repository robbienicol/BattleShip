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

The first frame never completes: it stalls inside
port_resume_service_threads() (log ends mid-thread-dump). The page stays
responsive, so wasm is parked in an Asyncify sleep rather than spinning.
Suspects, in order:
1. Something on a service thread sleeps through Asyncify inside a fiber
   (SDL_Delay → emscripten_sleep, e.g. audio queue pacing in the audio thread
   or controller polling). A sleep inside a fiber is not supported.
2. Fiber/Asyncify instrumentation gaps on indirect calls (an earlier run with
   emscripten_set_main_loop hit "table index is out of bounds" in
   sySchedulerThreadMain during a fiber rewind).

Next steps: build with -sASYNCIFY_DEBUG or log each fiber swap + SDL_Delay
call; make audio pacing non-blocking on the web; then measure Asyncify cost
and restrict instrumentation (ASYNCIFY_ADD/IGNORE_INDIRECT) if needed. Rollback
on the web also needs a fiber-aware coroutine pool save/load (currently the
pool falls back to plain coroutines and rollback saves report unsupported).
