#pragma once
// Safe, dependency-free WebAssembly binary reader. Treats its input as UNTRUSTED: every read is bounds
// checked and malformed files produce an error message, never a crash.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/diag.hpp"

namespace wf {

struct WasmSection {
  std::string name;  // "type", "import", "code", "custom:name", ...
  std::uint64_t bytes = 0;
};
struct WasmImport {
  std::string module, name, kind;  // kind: func | table | memory | global | tag
};
struct WasmExport {
  std::string name, kind;
};

struct WasmInfo {
  bool valid = false;
  std::string error;
  std::uint64_t fileBytes = 0;
  std::uint32_t version = 0;
  std::vector<WasmSection> sections;
  std::vector<WasmImport> imports;
  std::vector<WasmExport> exports;
  std::uint32_t functionsImported = 0, functionsDefined = 0;
  bool hasMemory = false, memoryImported = false, memoryShared = false, memoryHasMax = false, memory64 = false;
  std::uint64_t memoryMinPages = 0, memoryMaxPages = 0;
  std::uint64_t codeBytes = 0, dataBytes = 0;
  std::uint32_t dataSegments = 0;
  bool dataFullyParsed = true;
  bool hasStart = false;

  std::set<std::string> importModules() const;
  bool importsWasi() const;
  std::uint64_t sectionBytes(const std::string& name) const;
};

WasmInfo parseWasm(const std::string& bytes);

// "wasi" | "emscripten" | "standalone" | "host-imports" - what kind of host the module expects.
std::string classifyWasmTarget(const WasmInfo& w);
std::string describeWasm(const WasmInfo& w);
// Compares two builds: total size, per-section growth/shrinkage, import/export changes, memory.
std::string diffWasm(const WasmInfo& before, const WasmInfo& after);
// Problems that matter for the given kind of output (webTarget = must run in a browser).
std::vector<Diagnostic> wasmDiagnostics(const WasmInfo& w, bool webTarget);
}  // namespace wf
