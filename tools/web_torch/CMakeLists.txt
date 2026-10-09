# Torch for the browser: extracts BattleShip.o2r from the player's own ROM
# inside a Web Worker, so no game assets are ever hosted.
#
#   emcmake cmake -S tools/web_torch -B build-torch-web -GNinja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-torch-web
#
# Produces torch_runner.js/.wasm (exports torch_extract_o2r; see bridge.cpp).
cmake_minimum_required(VERSION 3.20)
project(torch_runner C CXX)
set(CMAKE_CXX_STANDARD 20)

# Same Torch configuration as the game build (SSB64 + NAudio only), built
# as a static library like the Android ROM importer.
set(USE_STANDALONE OFF CACHE BOOL "" FORCE)
set(BUILD_STORMLIB OFF CACHE BOOL "" FORCE)
set(BUILD_SM64 OFF CACHE BOOL "" FORCE)
set(BUILD_MK64 OFF CACHE BOOL "" FORCE)
set(BUILD_SF64 OFF CACHE BOOL "" FORCE)
set(BUILD_PM64 OFF CACHE BOOL "" FORCE)
set(BUILD_FZERO OFF CACHE BOOL "" FORCE)
set(BUILD_MARIO_ARTIST OFF CACHE BOOL "" FORCE)
set(BUILD_NAUDIO ON CACHE BOOL "" FORCE)
set(BUILD_SSB64 ON CACHE BOOL "" FORCE)

# Torch and yaml-cpp report errors by throwing. Torch's CMakeLists resets
# CMAKE_CXX_FLAGS, so enable wasm exceptions as a directory option instead.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>)

# Torch's Emscripten block adds embind/MODULARIZE flags to every link in its
# directory, which breaks FindThreads' C link probe. Emscripten's libc has
# the pthread API (single-threaded here), so answer the probe up front.
set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE INTERNAL "")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../torch ${CMAKE_BINARY_DIR}/torch)

# Torch's own embind bindings (src/lib/web.cpp) don't compile against the
# current Companion API; bridge.cpp replaces them.
get_target_property(TORCH_SOURCES torch SOURCES)
list(FILTER TORCH_SOURCES EXCLUDE REGEX "src/lib/web\\.cpp$")
set_property(TARGET torch PROPERTY SOURCES ${TORCH_SOURCES})

add_executable(torch_runner bridge.cpp)
target_include_directories(torch_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../torch/src)
target_link_libraries(torch_runner PRIVATE torch)
target_link_options(torch_runner PRIVATE
    -fwasm-exceptions
    -sMODULARIZE=1 -sEXPORT_NAME=createTorchRunner
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=4194304
    -sEXPORTED_FUNCTIONS=_torch_extract_o2r
    -sEXPORTED_RUNTIME_METHODS=FS,ccall)
