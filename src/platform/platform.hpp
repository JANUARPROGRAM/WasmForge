#pragma once
// OS abstraction layer. One implementation per OS family; everything above it is portable.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/util.hpp"

namespace wf {

struct ProcessResult {
  bool started = false;           // false => the program could not be launched (see `error`)
  int exitCode = -1;
  std::string output;             // stdout+stderr when capture=true (truncated at 4 MB)
  std::string error;
  double seconds = 0.0;
  std::uint64_t peakRssKb = 0;    // 0 = unknown on this platform
  bool timedOut = false;          // the process (and, on POSIX, its process group) was killed after timeoutSec
};

// Runs a program WITHOUT a shell. Arguments are passed as an argv vector, so user-controlled
// strings can never be interpreted as shell syntax. (Windows .bat/.cmd launchers are the only
// exception; see platform_win32.cpp for the strict validation applied there.)
ProcessResult runProcess(const std::vector<std::string>& argv, const fs::path& cwd = {}, bool capture = true, double timeoutSec = 0);

std::optional<fs::path> findInPath(const std::string& program);

struct MemoryInfo {
  bool ok = false;
  std::uint64_t totalBytes = 0;
  std::uint64_t availableBytes = 0;  // 0 = unknown
};
MemoryInfo queryMemory();             // honours cgroup limits when readable
unsigned queryCoreCount();            // 0 = unknown
std::uint64_t queryFreeDisk(const fs::path& where);  // 0 = unknown
std::string queryCpuModel();          // "UNKNOWN" when unavailable
std::string queryArch();              // x86_64 | arm64 | x86 | UNKNOWN ...
std::string osName();                 // linux | windows | macos | unknown
std::string envVar(const char* name); // "" when unset
fs::path userConfigDir();             // %APPDATA%\WasmForge, $XDG_CONFIG_HOME/wasmforge, ~/.config/wasmforge
}  // namespace wf
