#!/bin/sh
set -eu
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSEED_FETCH_SDL=OFF -DFETCHCONTENT_SOURCE_DIR_LZ4=/workspace/.deps/lz4-1.10.0
cmake --build build/linux --parallel 4
ctest --test-dir build/linux --output-on-failure
LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy xvfb-run -a \
    ./build/linux/seed_demo --smoke --damage-demo --verify-stream \
    --save build/linux-save --screenshot build/linux-island.ppm
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake \
    -DFETCHCONTENT_SOURCE_DIR_SDL2=/workspace/.deps/SDL2-2.30.11 \
    -DFETCHCONTENT_SOURCE_DIR_LZ4=/workspace/.deps/lz4-1.10.0 \
    -DSEED_HOST_COOKER=/workspace/build/linux/seed_cook
cmake --build build/windows --parallel 4
cmake --install build/windows --prefix /workspace/build/windows-distributable
