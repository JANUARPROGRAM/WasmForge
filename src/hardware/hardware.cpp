#include "hardware/hardware.hpp"

#include <sstream>

#include "platform/platform.hpp"

namespace wf {
namespace {
std::string detectEnvironment(const HardwareInfo& h) {
  if (h.os == "windows") return "windows";
  if (h.os == "macos") return "macos";
  if (!envVar("TERMUX_VERSION").empty() || envVar("PREFIX").find("com.termux") != std::string::npos) return "termux";
  if (!envVar("PROOT_TMP_DIR").empty() || !envVar("PROOT_L2S_DIR").empty()) return "proot";
  const std::string version = lower(readFile("/proc/version", 4096).value_or(""));
  bool traced = false;
  if (auto st = readFile("/proc/self/status", 1u << 16)) {
    const auto p = st->find("TracerPid:");
    if (p != std::string::npos) traced = std::atoi(st->c_str() + p + 10) > 0;
  }
  if (traced && version.find("android") != std::string::npos) return "proot";  // ptrace-based rootless chroot on Android
  if (!envVar("ANDROID_ROOT").empty()) return "android";
  if (version.find("microsoft") != std::string::npos) return "wsl";
  std::error_code ec;
  if (fs::exists("/.dockerenv", ec)) return "container";
  return "linux";
}
}  // namespace

HardwareInfo detectHardware(const fs::path& probeDir) {
  HardwareInfo h;
  h.os = osName();
  h.arch = queryArch();
  h.cpuModel = queryCpuModel();
  h.cores = queryCoreCount();
  const MemoryInfo m = queryMemory();
  if (m.ok) {
    h.ramTotal = m.totalBytes;
    h.ramAvailable = m.availableBytes;
  }
  std::error_code ec;
  fs::path probe = probeDir.empty() ? fs::current_path(ec) : probeDir;
  while (!probe.empty() && !fs::exists(probe, ec) && probe.has_parent_path() && probe != probe.parent_path())
    probe = probe.parent_path();
  h.diskFree = queryFreeDisk(probe);
  h.environment = detectEnvironment(h);
  if (h.cores == 0) h.notes.push_back("CPU core count could not be read; assuming 1 worker");
  if (h.ramTotal == 0) h.notes.push_back("RAM size could not be read; using a conservative memory budget");
  else if (h.ramAvailable == 0) h.notes.push_back("available RAM unknown; using a conservative memory budget");
  if (h.environment == "termux" || h.environment == "proot" || h.environment == "android")
    h.notes.push_back("Android host: a core is kept free and memory budget is reduced to avoid the phantom-process killer");
  return h;
}

std::string describeHardware(const HardwareInfo& h) {
  auto sz = [](std::uint64_t b) { return b ? humanBytes(b) : std::string("UNKNOWN"); };
  std::ostringstream o;
  o << "  os            " << h.os << " (" << h.environment << ")\n"
    << "  architecture  " << h.arch << "\n"
    << "  cpu           " << h.cpuModel << "\n"
    << "  cores         " << (h.cores ? std::to_string(h.cores) : "UNKNOWN") << "\n"
    << "  ram           " << sz(h.ramAvailable) << " available / " << sz(h.ramTotal) << " total\n"
    << "  disk free     " << sz(h.diskFree) << "\n";
  for (const auto& n : h.notes) o << "  note          " << n << "\n";
  return o.str();
}
}  // namespace wf
