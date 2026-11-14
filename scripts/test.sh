#!/usr/bin/env bash
# Builds wasmfrg + unit tests without CMake, then runs unit and integration tests.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
bash scripts/build.sh build
LIB=$(ls src/core/*.cpp src/platform/platform_posix.cpp src/hardware/*.cpp src/project/*.cpp src/assets/*.cpp \
         src/config/*.cpp src/backend/*.cpp src/build/*.cpp src/diagnostics/*.cpp src/runtime/*.cpp src/cli/*.cpp)
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Isrc $LIB tests/unit/test_main.cpp -o build/wf_unit_tests -pthread
./build/wf_unit_tests
bash tests/integration/run.sh
