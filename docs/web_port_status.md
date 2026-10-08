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

## Status (2026-10-08): runs at full speed

In the browser it boots, plays through the opening sequence at a steady
60 fps (~9 ms per frame: ~1 ms game logic, ~6 ms rendering). Fixes that got
it there:

- `-fwasm-exceptions` is incompatible with Asyncify — no exception catching
  in the web build (the one known throw, thread creation, is gone).
- C/C++ ABI: libultraship's `OSMesg` union is passed indirectly in wasm32, so
  C++ code must not call the C `osSendMesg` with it; the VRETRACE post goes
  through `port_post_vretrace()` in n64_stubs.c.
- Function-signature mismatches between declarations and definitions become
  traps in WebAssembly; fixed all four the linker reported (decomp
  `osVirtualToPhysical`, link bomb release, PK Thunder group id, viewport).
  Keep the link free of "function signature mismatch" warnings.
- `gfxPointerHasReadableBytes` used `mincore()` per SETTIMG — a slow JS
  syscall in the browser; it is a heap-bounds check there now.
- Texture fixups skip per-word work for textures already fixed at that size.

## Next

1. Input (keyboard/gamepad via SDL in the browser) and audio output checks
   on a visible page; asset extraction from the player's ROM in the browser
   (Torch has an Emscripten build mode) instead of preloading files.
2. Rollback on the web: fiber-aware coroutine pool save/load, WebRTC
   DataChannel transport for GekkoNet, hook into ssb64-web matchmaking.
3. Release build flags (drop -sASSERTIONS/--profiling-funcs), size/startup.
