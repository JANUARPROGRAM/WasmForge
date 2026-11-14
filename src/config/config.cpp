#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "platform/platform.hpp"

namespace wf {
namespace {
template <class E>
struct Name {
  const char* text;
  E value;
};
const Name<Profile> kProfiles[] = {{"auto", Profile::Auto},         {"low-memory", Profile::LowMemory},
                                   {"balanced", Profile::Balanced}, {"performance", Profile::Performance},
                                   {"custom", Profile::Custom}};
const Name<Strategy> kStrategies[] = {{"intelligent", Strategy::Intelligent}, {"compact", Strategy::Compact},
                                      {"execution", Strategy::Execution},     {"memory", Strategy::Memory},
                                      {"balanced", Strategy::Balanced},       {"none", Strategy::None}};
const Name<AssetMode> kAssets[] = {{"auto", AssetMode::Auto},         {"preserve", AssetMode::Preserve},
                                   {"embed", AssetMode::Embed},       {"external", AssetMode::External},
                                   {"transform", AssetMode::Transform}};
const Name<OutputKind> kOutputs[] = {{"web", OutputKind::Web}, {"wasm", OutputKind::WasmOnly},
                                     {"runtime", OutputKind::Runtime}, {"custom", OutputKind::Custom}};

template <class E, std::size_t N>
const char* nameOf(const Name<E> (&t)[N], E v) {
  for (const auto& n : t)
    if (n.value == v) return n.text;
  return "?";
}
template <class E, std::size_t N>
bool parseIn(const Name<E> (&t)[N], const std::string& s, E& out) {
  const std::string l = lower(trim(s));
  for (const auto& n : t)
    if (l == n.text) {
      out = n.value;
      return true;
    }
  return false;
}
bool parseUint(const std::string& s, std::uint64_t& out) {
  if (s.empty() || s.size() > 9) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  out = std::stoull(s);
  return true;
}
std::uint64_t clampU(std::uint64_t v, std::uint64_t lo, std::uint64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

const char* toString(Profile v) { return nameOf(kProfiles, v); }
const char* toString(Strategy v) { return nameOf(kStrategies, v); }
const char* toString(AssetMode v) { return nameOf(kAssets, v); }
const char* toString(OutputKind v) { return nameOf(kOutputs, v); }
bool parseEnum(const std::string& s, Profile& o) { return parseIn(kProfiles, s, o); }
bool parseEnum(const std::string& s, Strategy& o) { return parseIn(kStrategies, s, o); }
bool parseEnum(const std::string& s, AssetMode& o) { return parseIn(kAssets, s, o); }
bool parseEnum(const std::string& s, OutputKind& o) { return parseIn(kOutputs, s, o); }

std::vector<std::string> configKeys() {
  return {"profile", "workers", "memory", "strategy", "assets", "output", "backend", "html", "name"};
}

std::string configKeyHelp(const std::string& k) {
  if (k == "profile") return "auto | low-memory | balanced | performance | custom";
  if (k == "workers") return "parallel compile jobs (1-256)";
  if (k == "memory") return "memory budget for the build in MB (128-1048576)";
  if (k == "strategy") return "intelligent | compact | execution | memory | balanced | none";
  if (k == "assets") return "auto | preserve | embed | external";
  if (k == "output") return "web | wasm | runtime";
  if (k == "backend") return "auto | emscripten | wasi-sdk";
  if (k == "html") return "true | false (generate an HTML shell for web output)";
  if (k == "name") return "output base name (letters, digits, - and _)";
  return "";
}

bool validateKeyValue(const std::string& key, const std::string& rawValue, std::string& err) {
  const std::string value = trim(rawValue);
  std::uint64_t n = 0;
  Profile p;
  Strategy s;
  AssetMode a;
  OutputKind o;
  if (key == "profile") {
    if (parseEnum(value, p)) return true;
  } else if (key == "workers") {
    if (parseUint(value, n) && n >= 1 && n <= 256) return true;
  } else if (key == "memory") {
    if (parseUint(value, n) && n >= 128 && n <= 1048576) return true;
  } else if (key == "strategy") {
    if (parseEnum(value, s)) return true;
  } else if (key == "assets") {
    if (parseEnum(value, a)) {
      if (a == AssetMode::Transform) {
        err = "assets=transform is not implemented yet (assets are never converted); use preserve, embed, external or auto";
        return false;
      }
      return true;
    }
  } else if (key == "output") {
    if (parseEnum(value, o)) {
      if (o == OutputKind::Custom) {
        err = "output=custom is not implemented yet; use web, wasm or runtime";
        return false;
      }
      return true;
    }
  } else if (key == "backend") {
    if (value == "auto" || value == "emscripten" || value == "wasi-sdk") return true;
  } else if (key == "html") {
    if (value == "true" || value == "false") return true;
  } else if (key == "name") {
    bool ok = !value.empty() && value.size() <= 64;
    for (char c : value)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) ok = false;
    if (ok) return true;
  } else {
    err = "unknown key '" + key + "' (valid keys: " + [] {
      std::string j;
      for (const auto& k : configKeys()) j += (j.empty() ? "" : ", ") + k;
      return j;
    }() + ")";
    return false;
  }
  err = "invalid value '" + value + "' for '" + key + "' (expected: " + configKeyHelp(key) + ")";
  return false;
}

bool loadConfigFile(const fs::path& path, ConfigFile& out, std::vector<std::string>& issues) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return true;  // no file == empty layer
  auto txt = readFile(path, 1u << 20);
  if (!txt) {
    issues.push_back("cannot read config file " + pathStr(path));
    return false;
  }
  int lineNo = 0;
  for (const auto& raw : split(*txt, '\n')) {
    ++lineNo;
    const std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      issues.push_back(pathStr(path) + ":" + std::to_string(lineNo) + ": expected key = value");
      continue;
    }
    const std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
    std::string err;
    if (!validateKeyValue(k, v, err)) {
      issues.push_back(pathStr(path) + ":" + std::to_string(lineNo) + ": " + err);
      continue;
    }
    out[k] = v;
  }
  return true;
}

std::string exportConfig(const ConfigFile& cfg) {
  std::ostringstream o;
  o << "# WasmForge configuration\n";
  for (const auto& [k, v] : cfg) o << k << " = " << v << "\n";
  return o.str();
}

bool saveConfigFile(const fs::path& path, const ConfigFile& cfg) { return writeFileAtomic(path, exportConfig(cfg)); }

fs::path globalConfigPath() { return userConfigDir() / "config.ini"; }

ConfigMap mergeLayers(const ConfigFile& g, const ConfigFile& l, const ConfigFile& c) {
  ConfigMap m;
  for (const auto& [k, v] : g) m[k] = {v, "global"};
  for (const auto& [k, v] : l) m[k] = {v, "local"};
  for (const auto& [k, v] : c) m[k] = {v, "cli"};
  return m;
}

ResolvedConfig resolveConfig(const ConfigMap& cm, const HardwareInfo& hw, const ProjectInfo& proj) {
  ResolvedConfig rc;
  auto find = [&](const char* k) -> const ConfigValue* {
    auto it = cm.find(k);
    return it == cm.end() ? nullptr : &it->second;
  };
  auto add = [&](const std::string& k, const std::string& v, const std::string& src, const std::string& why) {
    rc.decisions.push_back({k, v, src, why});
  };
  auto mb = [](std::uint64_t n) { return std::to_string(n) + " MB"; };

  // ---- profile ------------------------------------------------------------------------------
  Profile requested = Profile::Auto;
  const ConfigValue* pv = find("profile");
  if (pv) parseEnum(pv->value, requested);
  rc.requestedProfile = requested;
  const bool memKnown = hw.ramAvailable > 0;
  const std::uint64_t availMb = hw.ramAvailable >> 20;
  const unsigned cores = hw.cores ? hw.cores : 1;
  const bool android = hw.environment == "termux" || hw.environment == "proot" || hw.environment == "android";
  Profile base = requested;
  std::string why;
  if (requested == Profile::Auto) {
    if (!memKnown || hw.cores == 0) {
      base = Profile::LowMemory;
      why = "RAM or core count is UNKNOWN, so the conservative LOW-MEMORY profile is used";
    } else if (availMb < 2048 || (android && availMb < 3072)) {
      base = Profile::LowMemory;
      why = "only " + mb(availMb) + " RAM available" + (android ? " on an Android host" : "");
    } else if (hw.cores >= 8 && availMb >= 8192) {
      base = Profile::Performance;
      why = std::to_string(hw.cores) + " cores and " + mb(availMb) + " RAM available";
    } else {
      base = Profile::Balanced;
      why = std::to_string(cores) + (cores == 1 ? " core and " : " cores and ") + mb(availMb) + " RAM available";
    }
  } else if (requested == Profile::Custom) {
    base = Profile::Balanced;
    why = "CUSTOM: values you do not set fall back to BALANCED";
  } else {
    why = "set explicitly";
  }
  rc.profile = base;
  add("profile", requested == Profile::Auto ? std::string("auto -> ") + toString(base) : toString(requested),
      pv ? pv->source : "auto", why);

  // ---- memory budget -------------------------------------------------------------------------
  rc.perJobMb = proj.hasCxx ? 700 : 350;
  std::uint64_t memMb = 512;
  std::string memWhy = "RAM unknown: fixed conservative budget";
  if (memKnown) {
    switch (base) {
      case Profile::LowMemory: memMb = clampU(availMb * 35 / 100, 256, 1536); memWhy = "35% of available RAM (capped at 1536 MB)"; break;
      case Profile::Performance: memMb = clampU(availMb * 70 / 100, 1024, 16384); memWhy = "70% of available RAM (capped at 16 GB)"; break;
      default: memMb = clampU(availMb * 50 / 100, 512, 8192); memWhy = "50% of available RAM (capped at 8 GB)"; break;
    }
  }
  std::string memSrc = "auto";
  if (const auto* v = find("memory")) {
    memMb = std::stoull(v->value);
    memSrc = v->source;
    memWhy = "set by you";
  }
  rc.memoryMb = memMb;
  add("memory", mb(memMb), memSrc, memWhy);

  // ---- workers -------------------------------------------------------------------------------
  unsigned wBase = 1;
  std::string wWhy = "core count UNKNOWN: single worker";
  if (hw.cores) {
    switch (base) {
      case Profile::LowMemory: wBase = std::min(2u, std::max(1u, cores / 2)); wWhy = "LOW-MEMORY: at most 2 workers"; break;
      case Profile::Performance: wBase = std::max(1u, cores - 1); wWhy = "PERFORMANCE: all cores but one stay free for the system"; break;
      default: wBase = std::max(1u, cores / 2); wWhy = "half of the cores, the rest stay free for other apps"; break;
    }
    if (android) wWhy += " (Android host: system and other apps need headroom)";
  }
  const unsigned byMem = static_cast<unsigned>(std::max<std::uint64_t>(1, memMb / rc.perJobMb));
  unsigned workers = std::min(wBase, byMem);
  if (byMem < wBase) wWhy += "; limited to " + std::to_string(byMem) + " by the memory budget (~" + std::to_string(rc.perJobMb) + " MB per compile job, estimate)";
  std::string wSrc = "auto";
  if (const auto* v = find("workers")) {
    workers = static_cast<unsigned>(std::stoul(v->value));
    wSrc = v->source;
    wWhy = "set by you";
    if (hw.cores && workers > hw.cores * 2) rc.warnings.push_back("workers=" + std::to_string(workers) + " is more than twice the core count (" + std::to_string(hw.cores) + ")");
    if (static_cast<std::uint64_t>(workers) * rc.perJobMb > memMb)
      rc.warnings.push_back("workers x ~" + std::to_string(rc.perJobMb) + " MB exceeds the " + mb(memMb) + " budget; the scheduler will throttle at run time");
  }
  rc.workers = workers;
  add("workers", std::to_string(workers), wSrc, wWhy);

  // ---- strategy ------------------------------------------------------------------------------
  Strategy st = Strategy::Intelligent;
  const ConfigValue* sv = find("strategy");
  if (sv) parseEnum(sv->value, st);
  std::string sWhy = "set by you";
  if (st == Strategy::Intelligent) {
    switch (base) {
      case Profile::LowMemory: st = Strategy::Compact; sWhy = "LOW-MEMORY: avoids aggressive inlining that raises compiler peak RAM and keeps output small"; break;
      case Profile::Performance: st = Strategy::Execution; sWhy = "PERFORMANCE profile: favours runtime speed"; break;
      default: st = Strategy::Balanced; sWhy = "BALANCED profile: speed/size trade-off"; break;
    }
    if (sv) sWhy = "INTELLIGENT resolved from the profile: " + sWhy;
  }
  rc.strategy = st;
  add("strategy", toString(st), sv ? sv->source : "auto", sWhy);

  // ---- assets / output / backend / name / html ---------------------------------------------------
  AssetMode am = AssetMode::Auto;
  const ConfigValue* av = find("assets");
  if (av) parseEnum(av->value, am);
  rc.assets = am;
  add("assets", toString(am), av ? av->source : "auto", av ? "set by you" : "decided per file (type, size, whether source code references it)");

  OutputKind ok = proj.hasEntry() ? OutputKind::Web : OutputKind::WasmOnly;
  const ConfigValue* ov = find("output");
  if (ov) parseEnum(ov->value, ok);
  rc.output = ok;
  add("output", toString(ok), ov ? ov->source : "auto",
      ov ? "set by you" : (proj.hasEntry() ? "project has a main(): browser package" : "no main() found: plain WASM module (library)"));

  if (const auto* v = find("backend")) rc.backend = v->value;
  add("backend", rc.backend, find("backend") ? find("backend")->source : "auto",
      rc.backend == "auto" ? "first available backend that supports the requested output" : "set by you");

  rc.name = proj.name.empty() ? "app" : proj.name;
  if (const auto* v = find("name")) rc.name = v->value;
  add("name", rc.name, find("name") ? find("name")->source : "auto", find("name") ? "set by you" : "project directory name");

  if (const auto* v = find("html")) rc.html = v->value == "true";
  add("html", rc.html ? "true" : "false", find("html") ? find("html")->source : "auto", find("html") ? "set by you" : "HTML shell is optional and off by default");

  // ---- cache budget -------------------------------------------------------------------------
  std::uint64_t cache = base == Profile::LowMemory ? 256 : (base == Profile::Performance ? 2048 : 1024);
  std::string cWhy = "by profile";
  if (hw.diskFree) {
    const std::uint64_t cap = std::max<std::uint64_t>(64, (hw.diskFree >> 20) / 4);
    if (cache > cap) {
      cache = cap;
      cWhy = "limited to 25% of free disk space";
    }
  }
  rc.cacheMb = cache;
  add("cache", mb(cache), "auto", cWhy);
  return rc;
}

std::string describeDecisions(const ResolvedConfig& rc) {
  std::ostringstream o;
  for (const auto& d : rc.decisions) {
    std::string k = d.key;
    k.resize(std::max<std::size_t>(k.size(), 9), ' ');
    std::string v = d.value;
    v.resize(std::max<std::size_t>(v.size(), 16), ' ');
    o << "  " << k << " " << v << " [" << d.source << "] " << d.reason << "\n";
  }
  for (const auto& w : rc.warnings) o << "  warning: " << w << "\n";
  return o.str();
}

}  // namespace wf
