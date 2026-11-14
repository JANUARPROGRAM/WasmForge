#include "diagnostics/doctor.hpp"

#include "backend/backend.hpp"
#include "config/config.hpp"
#include "hardware/hardware.hpp"
#include "platform/platform.hpp"

namespace wf {
namespace {
struct Counter {
  int pass = 0, warn = 0, fail = 0;
  void row(std::ostream& os, const char* tag, const std::string& what, const std::string& hint = "") {
    os << "  [" << tag << "] " << what << "\n";
    if (!hint.empty()) os << "         -> " << hint << "\n";
  }
  void ok(std::ostream& os, const std::string& w) { ++pass; row(os, "PASS", w); }
  void warning(std::ostream& os, const std::string& w, const std::string& h) { ++warn; row(os, "WARN", w, h); }
  void bad(std::ostream& os, const std::string& w, const std::string& h) { ++fail; row(os, "FAIL", w, h); }
};
}  // namespace

int runDoctor(std::ostream& os) {
  Counter c;
  const HardwareInfo hw = detectHardware(fs::current_path());
  os << "WasmForge doctor\n\nSystem\n" << describeHardware(hw) << "\nChecks\n";

  if (hw.arch == "UNKNOWN") c.warning(os, "CPU architecture is UNKNOWN", "conservative defaults will be used");
  else c.ok(os, "architecture " + hw.arch + " on " + hw.os + " (" + hw.environment + ")");
  if (hw.cores == 0) c.warning(os, "core count unreadable", "builds will use a single worker; set one with `wasmfrg config set workers N`");
  else c.ok(os, std::to_string(hw.cores) + " core(s) visible");
  if (hw.ramAvailable == 0) c.warning(os, "available RAM unreadable", "a fixed 512 MB budget is used; override with `wasmfrg config set memory <MB>`");
  else if ((hw.ramAvailable >> 20) < 1024) c.warning(os, "less than 1 GB RAM available", "LOW-MEMORY profile will be selected; close other apps for larger projects");
  else c.ok(os, humanBytes(hw.ramAvailable) + " RAM available");
  if (hw.diskFree == 0) c.warning(os, "free disk space unknown", "");
  else if (hw.diskFree < (1ull << 30)) c.warning(os, "less than 1 GB free disk space: " + humanBytes(hw.diskFree), "the object cache will be limited automatically");
  else c.ok(os, humanBytes(hw.diskFree) + " free disk space");

  const fs::path cfgDir = userConfigDir();
  std::error_code ec;
  fs::create_directories(cfgDir, ec);
  if (writeFileAtomic(cfgDir / ".doctor-probe", "ok")) { fs::remove(cfgDir / ".doctor-probe", ec); c.ok(os, "config directory writable: " + pathStr(cfgDir)); }
  else c.warning(os, "config directory not writable: " + pathStr(cfgDir), "`wasmfrg config set` will fail; project-local wasmforge.ini still works");
  const fs::path tmp = fs::temp_directory_path(ec) / "wasmfrg-doctor-probe";
  if (writeFileAtomic(tmp, "ok")) { fs::remove(tmp, ec); c.ok(os, "temporary directory writable"); }
  else c.bad(os, "temporary directory not writable", "set TMPDIR (POSIX) or TEMP (Windows) to a writable directory");

  int usable = 0;
  for (const auto& id : backendIds()) {
    auto b = makeBackend(id);
    const auto& p = b->probe();
    if (p.available) {
      ++usable;
      c.ok(os, b->displayName() + ": " + p.version);
    } else {
      c.warning(os, b->displayName() + ": unavailable", p.note);
    }
  }
  if (usable == 0) c.bad(os, "no WebAssembly backend is usable", "install Emscripten (browser targets) or the WASI SDK (WASI targets), then run `wasmfrg doctor` again");
  if (auto n = findInPath("node")) c.ok(os, "node found (" + pathStr(*n) + "): usable for runtime smoke tests");
  else c.warning(os, "node not found", "runtime validation of web builds needs Node.js (Phase 5)");
  if (!findInPath("wasm-opt")) os << "  [INFO] wasm-opt not in PATH (Emscripten bundles its own)\n";

  os << "\n" << c.pass << " passed, " << c.warn << " warning(s), " << c.fail << " failure(s)\n";
  return usable == 0 ? 3 : (c.fail ? 1 : 0);
}
}  // namespace wf
