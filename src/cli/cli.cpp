#include "cli/cli.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <set>
#include <thread>
#include <chrono>

#include "build/build.hpp"
#include "config/config.hpp"
#include "diagnostics/doctor.hpp"
#include "platform/platform.hpp"
#include "runtime/runner.hpp"
#include "runtime/server.hpp"
#include "runtime/wasm.hpp"

namespace wf {
namespace {

constexpr const char* kVersion = "0.1.0";

const char* kHelp = R"(wasmfrg - WasmForge: lightweight, hardware-aware C/C++ -> WebAssembly toolchain

Usage: wasmfrg <command> [path] [options]

Commands
  build [path]            analyse the project and build it (default path: .)
  gen-config [path]       show/write config headers generated from CMake/autotools templates (--dry-run: show only)
  analyze [path]          rebuild blast radius per header, include cycles, heavy sources (--touch <file>: what would recompile)
  compdb [path]           write compile_commands.json (for clangd / IDEs)
  inspect [path]          show what WasmForge understands about a project and why
  graph [path]            print the include/dependency graph in Graphviz DOT format
  optimize [path]         show the optimisation plan (what is done, and by which tool)
  assets inspect [path]   list non-source files with detected types (magic bytes)
  assets report [path]    show what happens to each asset and why
  run [path] [-- args]    run the built program (Node for Emscripten output, WASI runtime for .wasm); --timeout <s>
  dev [path]              watch + incremental rebuild + serve with live reload (--interval <ms>, --port, --isolate)
  serve [dir]             serve a build in the browser (http://localhost:8080); --port --bind --isolate
  benchmark [path]        measure cold and incremental builds (median of --runs)
  wasm-info <file.wasm>   analyse a WebAssembly binary (sections, imports, memory, Web-vs-WASI target)
  wasm-diff <a> <b>       compare two .wasm builds (size per section, imports/exports, memory)
  doctor                  check this machine and the available backends
  config show|auto|set|get|reset|export|import|validate
  clean [path]            remove .wasmforge/ and dist/ (--cache: only the object cache)
  version | help

Common options
  --profile <p>     auto | low-memory | balanced | performance | custom
  --workers <n>     parallel compile jobs         --memory <MB>   memory budget
  --strategy <s>    intelligent | compact | execution | memory | balanced | none
  --assets <m>      auto | preserve | embed | external
  --output <o>      web | wasm | runtime          --backend <b>   auto | emscripten | wasi-sdk
  --name <name>     output base name              --html          also emit an HTML shell
  --out <dir>       output directory (default: <path>/dist)
  --entry <file>    program entry when several files define main()
  --exclude <glob>  leave matching sources out (repeatable)
  --asset <glob>    package matching files even if no source references them (repeatable)
  --define <X[=Y]>  preprocessor define (repeatable)
  --var <NAME[=V]>  value for generated config headers (from config.h.in/.cin templates; repeatable)
  --budget <size>   (build) fail if a .wasm output is bigger, e.g. 2MB
  --flag <flag>     extra backend flag for compile and link (repeatable, never passed through a shell)
  --explain         print every automatic decision with its reason
  --dry-run         show the exact backend commands without running them
  --verify          (build) run the result afterwards as a smoke test; --timeout <s> (default 10)
  --force           ignore the object cache      --keep-going    do not stop at the first failing file
  --web             (wasm-info) treat the module as a browser target
  --verbose, -v     more detail                  --local         (config) write ./wasmforge.ini instead of the user config

Exit codes: 0 ok | 1 build/runtime failure | 2 usage error | 3 environment problem (no backend) | 4 configuration error
)";

struct Args {
  std::string cmd;
  std::vector<std::string> pos;
  std::map<std::string, std::vector<std::string>> opts;
  std::set<std::string> flags;
  std::vector<std::string> progArgs;  // everything after `--` (passed to the program by `run`)
};

const std::set<std::string>& valueOpts() {
  static const std::set<std::string> s = {"profile", "workers", "memory", "strategy", "assets", "output", "backend", "name", "out", "entry", "exclude", "asset", "define", "flag", "runs", "timeout", "port", "bind", "var", "interval", "budget", "touch"};
  return s;
}
const std::set<std::string>& boolOpts() {
  static const std::set<std::string> s = {"explain", "dry-run", "verbose", "force", "keep-going", "html", "local", "cache", "help", "version", "web", "verify", "isolate"};
  return s;
}

std::size_t editDistance(const std::string& a, const std::string& b) {
  std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

std::string suggest(const std::string& word, const std::vector<std::string>& choices) {
  std::string best;
  std::size_t bd = 3;
  for (const auto& c : choices) {
    const std::size_t d = editDistance(word, c);
    if (d < bd) { bd = d; best = c; }
  }
  return best;
}

bool parseArgs(const std::vector<std::string>& argv, Args& a, std::string& err) {
  for (std::size_t i = 1; i < argv.size(); ++i) {
    const std::string& s = argv[i];
    if (s == "--") {
      a.progArgs.assign(argv.begin() + static_cast<std::ptrdiff_t>(i) + 1, argv.end());
      break;
    }
    if (s == "-v") { a.flags.insert("verbose"); continue; }
    if (s == "-h") { a.flags.insert("help"); continue; }
    if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
      std::string key = s.substr(2), val;
      bool hasVal = false;
      const auto eq = key.find('=');
      if (eq != std::string::npos) { val = key.substr(eq + 1); key = key.substr(0, eq); hasVal = true; }
      if (valueOpts().count(key)) {
        if (!hasVal) {
          if (i + 1 >= argv.size()) { err = "option --" + key + " needs a value"; return false; }
          val = argv[++i];
        }
        a.opts[key].push_back(val);
      } else if (boolOpts().count(key)) {
        if (hasVal) { err = "option --" + key + " does not take a value"; return false; }
        a.flags.insert(key);
      } else {
        std::vector<std::string> all(valueOpts().begin(), valueOpts().end());
        all.insert(all.end(), boolOpts().begin(), boolOpts().end());
        const std::string s2 = suggest(key, all);
        err = "unknown option --" + key + (s2.empty() ? "" : " (did you mean --" + s2 + "?)");
        return false;
      }
    } else if (!s.empty() && s[0] == '-' && s.size() > 1) {
      err = "unknown option " + s;
      return false;
    } else if (a.cmd.empty()) {
      a.cmd = s;
    } else {
      a.pos.push_back(s);
    }
  }
  return true;
}

bool buildSession(const Args& a, std::size_t posIndex, SessionOptions& so, std::string& err) {
  so.project = fromUtf8(a.pos.size() > posIndex ? a.pos[posIndex] : ".");
  for (const char* k : {"profile", "workers", "memory", "strategy", "assets", "output", "backend", "name"}) {
    auto it = a.opts.find(k);
    if (it == a.opts.end()) continue;
    std::string e;
    if (!validateKeyValue(k, it->second.back(), e)) { err = e; return false; }
    so.cli[k] = trim(it->second.back());
  }
  if (a.flags.count("html")) so.cli["html"] = "true";
  auto get = [&](const char* k) -> const std::vector<std::string>* {
    auto it = a.opts.find(k);
    return it == a.opts.end() ? nullptr : &it->second;
  };
  if (auto v = get("out")) so.outDir = fromUtf8(v->back());
  if (auto v = get("entry")) so.entry = v->back();
  if (auto v = get("exclude")) so.excludes = *v;
  if (auto v = get("asset")) so.forceAssets = *v;
  if (auto v = get("define")) so.defines = *v;
  if (auto v = get("flag")) so.extraFlags = *v;
  if (auto v = get("var")) {
    for (const auto& x : *v) {
      std::string n, val;
      if (!parseVarAssignment(x, n, val)) { err = "invalid --var '" + x + "' (expected NAME or NAME=VALUE with a C identifier as NAME)"; return false; }
    }
    so.vars = *v;
  }
  return true;
}

int usageError(const std::string& msg) {
  std::cerr << "error: " << msg << "\n       run `wasmfrg help` for usage\n";
  return 2;
}

int cmdConfig(const Args& a) {
  const std::string sub = a.pos.empty() ? "show" : a.pos[0];
  const bool local = a.flags.count("local") > 0;
  const fs::path file = local ? fs::current_path() / "wasmforge.ini" : globalConfigPath();
  ConfigFile cfg;
  std::vector<std::string> issues;
  loadConfigFile(file, cfg, issues);
  auto save = [&]() {
    if (!saveConfigFile(file, cfg)) { std::cerr << "error: cannot write " << pathStr(file) << "\n"; return 1; }
    return 0;
  };
  if (sub == "set") {
    if (a.pos.size() != 3) return usageError("usage: wasmfrg config set <key> <value>");
    std::string err;
    const std::string key = lower(a.pos[1]);
    if (!validateKeyValue(key, a.pos[2], err)) { std::cerr << "error: " << err << "\n"; return 4; }
    cfg[key] = trim(a.pos[2]);
    const int r = save();
    if (!r) std::cout << key << " = " << cfg[key] << "  (" << pathStr(file) << ")\n";
    return r;
  }
  if (sub == "get") {
    if (a.pos.size() != 2) return usageError("usage: wasmfrg config get <key>");
    auto it = cfg.find(lower(a.pos[1]));
    if (it == cfg.end()) { std::cout << "(not set: automatic)\n"; return 0; }
    std::cout << it->second << "\n";
    return 0;
  }
  if (sub == "auto") {
    for (const char* k : {"profile", "workers", "memory"}) cfg.erase(k);
    const int r = save();
    if (!r) std::cout << "profile, workers and memory are automatic again (" << pathStr(file) << ")\n";
    return r;
  }
  if (sub == "reset") {
    std::error_code ec;
    fs::remove(file, ec);
    std::cout << "configuration reset (" << pathStr(file) << ")\n";
    return 0;
  }
  if (sub == "export") {
    if (a.pos.size() >= 2) {
      if (!saveConfigFile(fromUtf8(a.pos[1]), cfg)) { std::cerr << "error: cannot write " << a.pos[1] << "\n"; return 1; }
      std::cout << "exported to " << a.pos[1] << "\n";
    } else {
      std::cout << exportConfig(cfg);
    }
    return 0;
  }
  if (sub == "import") {
    if (a.pos.size() != 2) return usageError("usage: wasmfrg config import <file>");
    ConfigFile in;
    std::vector<std::string> iss;
    std::error_code ec;
    if (!fs::exists(fromUtf8(a.pos[1]), ec)) { std::cerr << "error: no such file: " << a.pos[1] << "\n"; return 1; }
    loadConfigFile(fromUtf8(a.pos[1]), in, iss);
    for (const auto& i : iss) std::cerr << "error: " << i << "\n";
    if (!iss.empty()) return 4;
    cfg = in;
    const int r = save();
    if (!r) std::cout << "imported " << cfg.size() << " setting(s)\n";
    return r;
  }
  if (sub == "validate") {
    for (const auto& i : issues) std::cerr << "error: " << i << "\n";
    if (!issues.empty()) return 4;
    std::cout << "configuration is valid (" << cfg.size() << " explicit setting(s) in " << pathStr(file) << ")\n";
    return 0;
  }
  if (sub == "show") {
    ConfigFile g, l;
    std::vector<std::string> iss;
    loadConfigFile(globalConfigPath(), g, iss);
    loadConfigFile(fs::current_path() / "wasmforge.ini", l, iss);
    const HardwareInfo hw = detectHardware(fs::current_path());
    const ProjectInfo none;
    const ResolvedConfig rc = resolveConfig(mergeLayers(g, l, {}), hw, none);
    std::cout << "Mode: " << (g.empty() && l.empty() ? "AUTO" : ((g.count("profile") || l.count("profile")) ? "MANUAL/HYBRID (profile set)" : "HYBRID (some values overridden, rest automatic)")) << "\n"
              << "Files: " << pathStr(globalConfigPath()) << ", ./wasmforge.ini\n\nHardware\n" << describeHardware(hw) << "\nEffective configuration (project-specific choices such as output depend on the project)\n"
              << describeDecisions(rc);
    for (const auto& i : iss) std::cerr << "warning: " << i << "\n";
    return 0;
  }
  return usageError("unknown config subcommand '" + sub + "' (show, auto, set, get, reset, export, import, validate)");
}

int cmdWasmInfo(const Args& a) {
  if (a.pos.size() != 1) return usageError("usage: wasmfrg wasm-info <file.wasm> [--web] [--verbose]");
  auto data = readFile(fromUtf8(a.pos[0]), 1u << 30);
  if (!data) { std::cerr << "error: cannot read " << a.pos[0] << " (missing, unreadable, or larger than 1 GB)\n"; return 1; }
  const WasmInfo w = parseWasm(*data);
  std::cout << a.pos[0] << "\n" << describeWasm(w);
  if (w.valid && a.flags.count("verbose")) {
    std::cout << "  imports:\n";
    for (const auto& i : w.imports) std::cout << "    " << i.kind << " " << i.module << "." << i.name << "\n";
    std::cout << "  exports:\n";
    for (const auto& e : w.exports) std::cout << "    " << e.kind << " " << e.name << "\n";
  }
  const auto ds = wasmDiagnostics(w, a.flags.count("web") > 0);
  printDiagnostics(std::cout, ds);
  for (const auto& d : ds)
    if (d.level == Level::Error) return 1;
  return 0;
}

double parseTimeout(const Args& a, double def) {
  auto it = a.opts.find("timeout");
  if (it == a.opts.end()) return def;
  const double v = std::atof(it->second.back().c_str());
  return v >= 1 && v <= 3600 ? v : def;
}

void printRun(const RunResult& r, const std::string& indent) {
  std::cout << indent << "runtime " << (r.runtime.empty() ? "(none)" : r.runtime) << ": ";
  if (!r.started) { std::cout << "could not start\n"; }
  else if (r.timedOut) std::cout << "still running after " << std::fixed << std::setprecision(1) << r.seconds << " s (killed)\n";
  else std::cout << "exit code " << r.exitCode << " after " << std::fixed << std::setprecision(2) << r.seconds << " s\n";
  if (!r.output.empty()) {
    auto lines = split(r.output, '\n');
    const std::size_t from = lines.size() > 25 ? lines.size() - 25 : 0;
    for (std::size_t i = from; i < lines.size(); ++i)
      if (i + 1 < lines.size() || !lines[i].empty()) std::cout << indent << "| " << lines[i] << "\n";
  }
  if (!r.note.empty()) std::cout << indent << r.note << "\n";
}

uint64_t treeFingerprint(const fs::path& root, const fs::path& outDir) {
  std::uint64_t h = 1469598103934665603ULL;
  std::error_code ec;
  const fs::path skipOut = fs::weakly_canonical(outDir, ec);
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
  while (!ec && it != end) {
    std::error_code e2;
    const bool link = it->is_symlink(e2);
    if (!link && it->is_directory(e2)) {
      if (shouldSkipDirName(it->path().filename().string()) || fs::weakly_canonical(it->path(), e2) == skipOut) it.disable_recursion_pending();
    } else if (!link && it->is_regular_file(e2)) {
      const std::string name = it->path().filename().string();
      const bool junk = startsWith(name, ".#") || endsWith(name, ".swp") || endsWith(name, ".tmp") || endsWith(name, "~") || endsWith(name, ".o") || endsWith(name, ".swx");
      if (!junk) {
        h = fnv1a(pathStr(it->path().lexically_relative(root)), h);
        const auto t = static_cast<std::int64_t>(it->last_write_time(e2).time_since_epoch().count());
        const auto sz = static_cast<std::uint64_t>(it->file_size(e2));
        h = fnv1a(&t, sizeof t, h);
        h = fnv1a(&sz, sizeof sz, h);
      }
    }
    it.increment(ec);
  }
  return h;
}

int cmdDev(const Args& a, const SessionOptions& so) {
  using namespace std::chrono;
  BuildOptions bo;
  bo.session = so;
  bo.keepGoing = a.flags.count("keep-going") > 0;
  bo.verbose = a.flags.count("verbose") > 0;
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(fs::absolute(so.project), ec);
  const fs::path outDir = so.outDir.empty() ? root / "dist" : fs::absolute(so.outDir);
  int interval = detectHardware(root).environment == "termux" ? 1000 : 500;  // gentler on phone batteries
  if (a.opts.count("interval")) {
    const int v = std::atoi(a.opts.at("interval").back().c_str());
    if (v < 100 || v > 10000) return usageError("--interval must be between 100 and 10000 (milliseconds)");
    interval = v;
  }
  std::atomic<int> stamp{0};
  std::cout << "[dev] initial build\n";
  const BuildReport first = runBuild(bo, std::cout);
  if (first.ok) stamp = 1;
  else std::cout << "[dev] the first build failed; the server still starts and rebuilds when you fix the problem\n";
  fs::create_directories(outDir, ec);

  ServeOptions sv;
  sv.root = outDir;
  sv.stamp = [&stamp]() { return std::to_string(stamp.load()); };
  if (a.opts.count("port")) {
    const std::string& ps = a.opts.at("port").back();
    const int pn = std::atoi(ps.c_str());
    if (ps.empty() || pn < 0 || pn > 65535 || (pn == 0 && ps != "0")) return usageError("--port must be 0-65535");
    sv.port = pn;
  }
  if (a.opts.count("bind")) sv.bind = a.opts.at("bind").back();
  sv.isolate = a.flags.count("isolate") > 0;
  std::atomic<bool> serverDone{false};
  int serverRc = 0;
  std::thread srv([&]() { serverRc = runServer(sv, std::cout); serverDone = true; });

  std::uint64_t last = treeFingerprint(root, outDir);
  int idleTicks = 0;
  while (!serverStopRequested() && !serverDone) {
    const int wait = idleTicks > 600 ? std::max(interval, 1500) : interval;  // back off after a long quiet period
    std::this_thread::sleep_for(milliseconds(wait));
    std::uint64_t now = treeFingerprint(root, outDir);
    if (now == last) { ++idleTicks; continue; }
    for (int i = 0; i < 10 && !serverStopRequested(); ++i) {  // debounce: wait until the editor finished writing
      std::this_thread::sleep_for(milliseconds(100));
      const std::uint64_t n2 = treeFingerprint(root, outDir);
      if (n2 == now) break;
      now = n2;
    }
    last = now;
    idleTicks = 0;
    std::cout << "[dev] change detected, rebuilding" << std::endl;
    const auto t0 = steady_clock::now();
    const BuildReport r = runBuild(bo, std::cout);
    const double secs = duration<double>(steady_clock::now() - t0).count();
    if (r.ok) {
      ++stamp;
      std::cout << "[dev] rebuilt in " << std::fixed << std::setprecision(2) << secs << " s - open pages reload automatically" << std::endl;
    } else {
      std::cout << "[dev] build failed - still serving the last good build" << std::endl;
    }
  }
  requestServerStop();
  srv.join();
  return serverRc;
}

int cmdWasmDiff(const Args& a) {
  if (a.pos.size() != 2) return usageError("usage: wasmfrg wasm-diff <before.wasm> <after.wasm>");
  auto x = readFile(fromUtf8(a.pos[0]), 1u << 30), y = readFile(fromUtf8(a.pos[1]), 1u << 30);
  if (!x || !y) { std::cerr << "error: cannot read " << (!x ? a.pos[0] : a.pos[1]) << "\n"; return 1; }
  const WasmInfo wa = parseWasm(*x), wb = parseWasm(*y);
  std::cout << a.pos[0] << " -> " << a.pos[1] << "\n" << diffWasm(wa, wb);
  return wa.valid && wb.valid ? 0 : 1;
}

int cmdBenchmark(const Args& a, const SessionOptions& so) {
  int runs = 3;
  if (a.opts.count("runs")) {
    const int n = std::atoi(a.opts.at("runs").back().c_str());
    if (n < 1 || n > 50) return usageError("--runs must be between 1 and 50");
    runs = n;
  }
  std::vector<double> coldTotal, coldCompile, coldLink, warm;
  std::uint64_t rss = 0;
  std::ostringstream sink;
  BuildOptions bo;
  bo.session = so;
  for (int i = 0; i < runs; ++i) {
    cleanProject(so, true, sink);
    bo.force = false;
    sink.str("");
    const BuildReport r = runBuild(bo, sink);
    if (!r.ok) { std::cout << sink.str() << "benchmark aborted: the build failed\n"; return r.exitCode ? r.exitCode : 1; }
    coldTotal.push_back(r.totalSeconds); coldCompile.push_back(r.compileSeconds); coldLink.push_back(r.linkSeconds);
    rss = std::max(rss, r.peakChildRssKb);
    sink.str("");
    const BuildReport w = runBuild(bo, sink);
    if (!w.ok) { std::cout << sink.str() << "benchmark aborted: the incremental build failed\n"; return 1; }
    warm.push_back(w.totalSeconds);
  }
  std::cout << std::fixed << std::setprecision(3) << "Benchmark (" << runs << " run(s), median)\n"
            << "  cold build         " << median(coldTotal) << " s   (backend compile " << median(coldCompile) << " s, backend link " << median(coldLink)
            << " s, WasmForge " << std::max(0.0, median(coldTotal) - median(coldCompile) - median(coldLink)) << " s)\n"
            << "  incremental (no-op) " << median(warm) << " s\n";
  if (rss) std::cout << "  peak RAM           " << humanBytes(rss * 1024) << " (largest backend process)\n";
  std::cout << "Only measurements made on this machine are reported. Comparison against other toolchains is not implemented;\n"
               "the backend/WasmForge split above shows what WasmForge itself contributes.\n";
  return 0;
}

}  // namespace

int runCli(const std::vector<std::string>& argv) {
  Args a;
  std::string err;
  if (!parseArgs(argv, a, err)) return usageError(err);
  if (a.cmd.empty() || a.cmd == "help" || a.flags.count("help")) { std::cout << kHelp; return a.cmd.empty() && !a.flags.count("help") ? 2 : 0; }
  if (a.cmd == "version" || a.flags.count("version")) {
    std::cout << "wasmfrg " << kVersion << " (WasmForge)\nplatform: " << osName() << "/" << queryArch() << "\n";
    return 0;
  }
  if (a.cmd == "doctor") return runDoctor(std::cout);
  if (a.cmd == "config") return cmdConfig(a);
  if (a.cmd == "wasm-info") return cmdWasmInfo(a);
  if (a.cmd == "wasm-diff") return cmdWasmDiff(a);

  static const std::vector<std::string> cmds = {"build", "inspect", "graph", "optimize", "assets", "benchmark", "doctor", "config", "clean", "version", "help", "wasm-info", "run", "serve", "gen-config", "analyze", "compdb", "dev", "wasm-diff"};
  if (std::find(cmds.begin(), cmds.end(), a.cmd) == cmds.end()) {
    const std::string s = suggest(a.cmd, cmds);
    return usageError("unknown command '" + a.cmd + "'" + (s.empty() ? "" : " (did you mean '" + s + "'?)"));
  }

  SessionOptions so;
  std::size_t posIndex = 0;
  std::string sub;
  if (a.cmd == "assets") {
    if (a.pos.empty() || (a.pos[0] != "inspect" && a.pos[0] != "report")) return usageError("usage: wasmfrg assets inspect|report [path]");
    sub = a.pos[0];
    posIndex = 1;
  }
  if (!buildSession(a, posIndex, so, err)) { std::cerr << "error: " << err << "\n"; return 4; }

  if (a.cmd == "clean") return cleanProject(so, a.flags.count("cache") > 0, std::cout) ? 0 : 1;
  if (a.cmd == "benchmark") return cmdBenchmark(a, so);
  if (a.cmd == "dev") return cmdDev(a, so);
  if (a.cmd == "serve") {
    ServeOptions sv;
    sv.root = so.outDir.empty() ? so.project : so.outDir;
    std::error_code ec;
    if (so.outDir.empty() && fs::is_directory(so.project / "dist", ec)) sv.root = so.project / "dist";
    if (a.opts.count("port")) {
      const std::string& ps = a.opts.at("port").back();
      const int pn = std::atoi(ps.c_str());
      if (ps.empty() || pn < 0 || pn > 65535 || (pn == 0 && ps != "0")) return usageError("--port must be 0-65535");
      sv.port = pn;
    }
    if (a.opts.count("bind")) sv.bind = a.opts.at("bind").back();
    sv.isolate = a.flags.count("isolate") > 0;
    return runServer(sv, std::cout);
  }
  if (a.cmd == "run") {
    Session S;
    prepareSession(so, S);
    RunOptions ro;
    ro.outDir = S.outDir; ro.name = S.rc.name; ro.genDir = S.root / ".wasmforge" / "gen";
    ro.timeoutSec = parseTimeout(a, 3600); ro.args = a.progArgs;
    const RunResult rr = runBuilt(ro);
    if (!rr.found || !rr.started) { std::cerr << "error: " << (rr.note.empty() ? rr.output : rr.note) << "\n"; return rr.found ? 3 : 1; }
    std::cout << rr.output;
    if (rr.timedOut) { std::cerr << "wasmfrg: stopped after the time limit (" << rr.runtime << ")\n"; return 124; }
    return rr.exitCode;
  }

  if (a.cmd == "gen-config") {
    Session S;
    prepareSession(so, S);
    printGeneratedHeaders(std::cout, S, a.flags.count("verbose") > 0);
    if (S.generated.empty() || a.flags.count("dry-run")) return 0;
    std::string gerr;
    if (!writeGeneratedHeaders(S, gerr)) { std::cerr << "error: " << gerr << "\n"; return 1; }
    std::cout << "written under " << pathStr(S.root / ".wasmforge" / "gen" / "include") << " (original sources untouched)\n";
    return 0;
  }
  if (a.cmd == "build") {
    BuildOptions bo;
    bo.session = so;
    if (a.opts.count("budget")) {
      std::string b = lower(trim(a.opts.at("budget").back()));
      std::uint64_t mult = 1;
      for (auto [suf, m] : {std::pair<const char*, std::uint64_t>{"gb", 1ull << 30}, {"mb", 1ull << 20}, {"kb", 1ull << 10}, {"b", 1}})
        if (endsWith(b, suf)) { mult = m; b = trim(b.substr(0, b.size() - std::string(suf).size())); break; }
      const double n = std::atof(b.c_str());
      if (b.empty() || n <= 0) return usageError("--budget expects a size such as 2MB, 500KB or 1048576");
      bo.budgetBytes = static_cast<std::uint64_t>(n * static_cast<double>(mult));
    }
    bo.dryRun = a.flags.count("dry-run") > 0;
    bo.explain = a.flags.count("explain") > 0;
    bo.verbose = a.flags.count("verbose") > 0;
    bo.force = a.flags.count("force") > 0;
    bo.keepGoing = a.flags.count("keep-going") > 0;
    const BuildReport r = runBuild(bo, std::cout);
    if (!r.ok) return r.exitCode ? r.exitCode : 1;
    if (a.flags.count("verify") && !bo.dryRun) {
      RunOptions ro;
      ro.outDir = r.outDir; ro.name = r.name; ro.genDir = r.genDir; ro.timeoutSec = parseTimeout(a, 10);
      const RunResult rr = runBuilt(ro);
      std::cout << "\nRuntime smoke test\n";
      printRun(rr, "  ");
      if (!rr.started) { std::cout << "  result: NOT VERIFIED (could not run here)\n"; return 0; }
      if (rr.timedOut) { std::cout << "  result: no crash within the time limit (normal for interactive programs); behaviour not verified\n"; return 0; }
      if (rr.exitCode != 0) { std::cout << "  result: FAILED (non-zero exit)\n"; return 1; }
      std::cout << "  result: PASSED (ran and exited 0). Note: this does not prove correct output in a browser.\n";
    }
    return 0;
  }

  Session S;
  prepareSession(so, S);
  if (a.cmd == "analyze") {
    std::string touch;
    if (a.opts.count("touch")) touch = a.opts.at("touch").back();
    printAnalysis(std::cout, S, touch);
    return 0;
  }
  if (a.cmd == "compdb") {
    const fs::path outp = so.outDir.empty() ? S.root / ".wasmforge" / "compile_commands.json" : so.outDir;
    std::string cerr2;
    if (!writeCompileDb(S, outp, cerr2)) { std::cerr << "error: " << cerr2 << "\n"; return 1; }
    std::cout << "wrote " << pathStr(outp) << " (" << S.proj.sources.size() << " entries)\n  for clangd: --compile-commands-dir=" << pathStr(outp.parent_path()) << "\n";
    return 0;
  }
  if (a.cmd == "inspect") {
    printSession(std::cout, S, a.flags.count("explain") > 0);
    return S.hasError() && !S.hasError("E-NOBACKEND") ? 1 : 0;
  }
  if (a.cmd == "graph") {
    if (a.opts.count("out")) {
      std::ostringstream o;
      printGraphDot(o, S);
      if (!writeFileAtomic(so.outDir, o.str())) { std::cerr << "error: cannot write " << pathStr(so.outDir) << "\n"; return 1; }
      std::cout << "graph written to " << pathStr(so.outDir) << "\n";
    } else {
      printGraphDot(std::cout, S);
    }
    return 0;
  }
  if (a.cmd == "optimize") {
    printOptimizationPlan(std::cout, S);
    return 0;
  }
  if (a.cmd == "assets") {
    printAssetReport(std::cout, S, sub == "report");
    return 0;
  }
  return usageError("unhandled command");
}
}  // namespace wf
