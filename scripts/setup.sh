#!/usr/bin/env bash
# Configure, compile, and test WasmForge on the current POSIX host.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v cmake >/dev/null 2>&1; then
  echo "[WasmForge] CMake tidak ditemukan. Install CMake dulu lewat package manager OS lu." >&2
  exit 2
fi
if ! command -v c++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
  echo "[WasmForge] Compiler C++17 tidak ditemukan. Install GCC atau Clang dulu." >&2
  exit 2
fi

BUILD_DIR="${WASMFORGE_BUILD_DIR:-build}"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DWASMFORGE_BUILD_TESTS=ON
cmake --build "$BUILD_DIR" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ctest --test-dir "$BUILD_DIR" --output-on-failure
mkdir -p bin
cp "$BUILD_DIR/wasmfrg" bin/wasmfrg
chmod +x bin/wasmfrg
printf '\n[WasmForge] Selesai. Binary: %s/bin/wasmfrg\n' "$ROOT"
printf '[WasmForge] Jalankan: %s/bin/wasmfrg --help\n' "$ROOT"
