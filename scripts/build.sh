#!/usr/bin/env bash
# Builds wasmfrg without CMake (handy on Termux / minimal systems). Needs only a C++17 compiler.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
OUT="${1:-build}"
mkdir -p "$OUT"
SRC=$(ls src/core/*.cpp src/platform/platform_posix.cpp src/hardware/*.cpp src/project/*.cpp src/assets/*.cpp \
         src/config/*.cpp src/backend/*.cpp src/build/*.cpp src/diagnostics/*.cpp src/runtime/*.cpp src/cli/*.cpp)
"$CXX" -std=c++17 -O2 -Wall -Wextra -Isrc $SRC src/main.cpp -o "$OUT/wasmfrg" -pthread
echo "built $OUT/wasmfrg"
