#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
seed_configuration=${1:-release}
case "$seed_configuration" in debug|release) ;; *) echo 'Choose debug or release' >&2; exit 2;; esac
seed_cmake=cmake
if ! command -v cmake >/dev/null 2>&1; then
    seed_cmake="$PWD/.tools/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake"
fi
if [ -d .deps/SDL2-2.30.11 ] && [ -d .deps/lz4-1.10.0 ] && [ -d .deps/imgui-1.92.9b ]; then
    "$seed_cmake" --preset "$seed_configuration" \
        -DFETCHCONTENT_SOURCE_DIR_SDL2="$PWD/.deps/SDL2-2.30.11" \
        -DFETCHCONTENT_SOURCE_DIR_LZ4="$PWD/.deps/lz4-1.10.0" \
        -DFETCHCONTENT_SOURCE_DIR_IMGUI="$PWD/.deps/imgui-1.92.9b"
else
    "$seed_cmake" --preset "$seed_configuration"
fi
"$seed_cmake" --build --preset "$seed_configuration" --parallel 4
