#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/util.hpp"

namespace wf {

// Everything here is best-effort and rootless. Unreadable values stay "UNKNOWN" / 0 and the
// configuration engine then falls back to conservative strategies.
struct HardwareInfo {
  std::string os = "UNKNOWN";
  std::string arch = "UNKNOWN";
  std::string cpuModel = "UNKNOWN";
  std::string environment = "UNKNOWN";  // termux | proot | android | wsl | container | linux | windows | macos
  unsigned cores = 0;                   // 0 = unknown
  std::uint64_t ramTotal = 0;           // bytes, 0 = unknown
  std::uint64_t ramAvailable = 0;       // bytes, 0 = unknown
  std::uint64_t diskFree = 0;           // bytes, 0 = unknown
  std::vector<std::string> notes;
};

HardwareInfo detectHardware(const fs::path& probeDir);
std::string describeHardware(const HardwareInfo& h);
}  // namespace wf
