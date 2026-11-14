#pragma once
#include <string>
#include <vector>

#include "core/util.hpp"

namespace wf {

struct RunOptions {
  fs::path outDir;      // directory holding <name>.js / <name>.wasm
  std::string name;
  fs::path genDir;      // where helper scripts may be written (.wasmforge/gen)
  double timeoutSec = 10;
  std::vector<std::string> args;
};

struct RunResult {
  bool found = false;      // an output to run exists
  bool started = false;
  bool timedOut = false;
  int exitCode = -1;
  double seconds = 0;
  std::string runtime;     // "node" | "wasmtime" | "wasmer" | "node:wasi"
  std::string output;
  std::string note;        // why nothing ran / what to do
};

// Runs a built program: Emscripten glue (<name>.js) with Node, or a plain WASI module (<name>.wasm) with
// wasmtime/wasmer if present, else Node's built-in WASI. Programs that need a canvas/DOM cannot run here.
RunResult runBuilt(const RunOptions& o);
}  // namespace wf
