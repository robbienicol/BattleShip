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

## Rollback snapshots in the browser (2026-10-08): verified

- Game globals: wasm-ld emits no section start/stop symbols, but keeps the
  custom `ssbdata`/`ssbbss` segments together in link order, so
  port/rollback/markers/begin.c and end.c (first and last ssb64_game
  sources) bracket them. Engine plumbing stays in normal .data/.bss.
- Coroutines: pooled fibers (port/coroutine_emscripten.cpp) keep their C
  stack and Asyncify buffer for life; a snapshot copies the fiber struct, the
  live C stack (stack_ptr..top) and the used Asyncify buffer.
- Sync test in the browser (depth 7, every tick, 1800-frame replay):
  12,915 rollbacks, 0 hash/byte mismatches, replay verify PASS. Save
  ~0.04 ms, load ~0.16 ms, 7-frame re-simulation ~1.2 ms, state ~1.2 MB.

Run it: build-web/test.html?SSB64_SYNCTEST=7&SSB64_REPLAY_PLAY=/test.ssb64r&preload=test.ssb64r
(make the replay with debug_tools/rollback/make_replay.py build-web/test.ssb64r).

## Next

1. WebRTC DataChannel transport for GekkoNet; hook into ssb64-web
   matchmaking (site picks fighters/stage, game jumps into the battle).
2. Input (keyboard/gamepad via SDL in the browser) and audio output checks
   on a visible page; asset extraction from the player's ROM in the browser
   (Torch has an Emscripten build mode) instead of preloading files.
3. Release build flags (drop -sASSERTIONS/--profiling-funcs), size/startup.
