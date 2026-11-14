#include "build/build.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <sstream>
#include <thread>

#include "platform/platform.hpp"
#include "runtime/wasm.hpp"

namespace wf {

bool Session::hasError(const std::string& code) const {
  for (const auto& d : diags)
    if (d.level == Level::Error && (code.empty() || d.code == code)) return true;
  return false;
}

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string o;
  for (const auto& s : v) o += (o.empty() ? "" : sep) + s;
  return o;
}

std::set<std::string> activeFiles(const Session& S) {
  std::set<std::string> a;
  for (const auto& s : S.proj.sources) {
    a.insert(s.rel);
    for (const auto& h : transitiveHeaders(S.proj, s.rel)) a.insert(h);
  }
  return a;
}

std::string quoteForDisplay(const std::string& s) {
  if (!s.empty() && s.find_first_of(" \t\"'$&|;<>()") == std::string::npos) return s;
  return "'" + s + "'";
}
std::string displayArgv(const std::vector<std::string>& a) {
  std::string o;
  for (const auto& s : a) o += (o.empty() ? "" : " ") + quoteForDisplay(s);
  return o;
}

void applySourceFilters(Session& S) {
  auto& src = S.proj.sources;
  for (const auto& pat : S.opts.excludes) {
    src.erase(std::remove_if(src.begin(), src.end(),
                             [&](const SourceFile& f) {
                               return globMatch(pat, f.rel) || (pat.find('/') == std::string::npos && globMatch(pat, fs::path(f.rel).filename().string()));
                             }),
              src.end());
  }
  if (src.empty()) {
    S.diags.push_back({Level::Error, "E-NOSOURCE", "no C/C++ source files to build" + std::string(S.opts.excludes.empty() ? "" : " (after --exclude)"),
                       "Point wasmfrg at a directory that contains .c/.cpp files."});
    return;
  }
  std::vector<std::string> mains;
  for (const auto& f : src)
    if (f.hasMain) mains.push_back(f.rel);
  if (!S.opts.entry.empty()) {
    std::string chosen;
    for (const auto& f : src)
      if (f.rel == S.opts.entry || endsWith(f.rel, "/" + S.opts.entry)) chosen = f.rel;
    if (chosen.empty()) {
      S.diags.push_back({Level::Error, "E-ENTRY", "entry file '" + S.opts.entry + "' is not one of the selected sources", "Run `wasmfrg inspect` to list the sources and entry points."});
      return;
    }
    src.erase(std::remove_if(src.begin(), src.end(), [&](const SourceFile& f) { return f.hasMain && f.rel != chosen; }), src.end());
  } else if (mains.size() > 1) {
    std::string list;
    for (std::size_t i = 0; i < mains.size() && i < 8; ++i) list += (i ? ", " : "") + mains[i];
    S.diags.push_back({Level::Error, "E-MULTIMAIN", std::to_string(mains.size()) + " files define main(): " + list + (mains.size() > 8 ? ", ..." : ""),
                       "Choose one program with --entry <file>, or leave the others out with --exclude <glob>."});
  }
}

void chooseBackend(Session& S) {
  const std::string wanted = S.cm.count("backend") ? S.cm.at("backend").value : "auto";
  OutputKind wantedKind = OutputKind::Web;
  const bool kindGiven = S.cm.count("output") > 0;
  if (kindGiven) parseEnum(S.cm.at("output").value, wantedKind);
  else if (!S.proj.hasEntry()) wantedKind = OutputKind::WasmOnly;
  if (wanted != "auto") {
    S.backend = makeBackend(wanted);
    return;
  }
  for (const auto& id : backendIds()) {
    auto b = makeBackend(id);
    if (b->probe().available && b->supports(wantedKind)) {
      S.backend = std::move(b);
      return;
    }
  }
  S.backend = makeBackend("emscripten");  // kept for planning (--dry-run, inspect); probe says "unavailable"
}
}  // namespace

bool prepareSession(const SessionOptions& so, Session& S) {
  S.opts = so;
  std::error_code ec;
  S.root = fs::weakly_canonical(fs::absolute(so.project), ec);
  if (ec) S.root = fs::absolute(so.project);
  S.outDir = so.outDir.empty() ? S.root / "dist" : fs::absolute(so.outDir);

  DiscoverOptions d;
  d.skipPaths.push_back(S.outDir);
  std::string err;
  if (!discoverProject(S.root, d, S.proj, err)) {
    S.diags.push_back({Level::Error, "E-PROJECT", err, "Pass an existing project directory, e.g. `wasmfrg build ./project`."});
    return false;
  }
  S.hw = detectHardware(S.root);

  ConfigFile g, l;
  std::vector<std::string> issues;
  loadConfigFile(globalConfigPath(), g, issues);
  loadConfigFile(S.root / "wasmforge.ini", l, issues);
  for (const auto& i : issues) S.diags.push_back({Level::Warning, "W-CONFIG", i, "Fix or remove the offending line (`wasmfrg config validate`)."});
  S.cm = mergeLayers(g, l, so.cli);
  for (const auto& [k, v] : S.cm) {
    std::string e;
    if (!validateKeyValue(k, v.value, e)) S.diags.push_back({Level::Error, "E-CONFIG", e, "Check `wasmfrg config show`."});
  }
  for (const auto& w : S.proj.warnings) S.diags.push_back({Level::Warning, "W-PROJECT", w, ""});

  applySourceFilters(S);
  chooseBackend(S);
  S.rc = resolveConfig(S.cm, S.hw, S.proj);

  const BackendProbe& pr = S.backend->probe();
  if (!pr.available) {
    S.diags.push_back({Level::Error, "E-NOBACKEND", "backend '" + S.backend->id() + "' is not available: " + pr.note,
                       "Run `wasmfrg doctor` for a full environment check."});
  }
  if (!S.backend->supports(S.rc.output)) {
    if (!S.cm.count("output") && S.backend->supports(OutputKind::WasmOnly)) {
      S.rc.output = OutputKind::WasmOnly;
      S.rc.decisions.push_back({"output", "wasm", "auto", "backend '" + S.backend->id() + "' cannot produce browser packages (it targets WASI)"});
    } else {
      S.diags.push_back({Level::Error, "E-TARGET", "backend '" + S.backend->id() + "' cannot produce output '" + std::string(toString(S.rc.output)) + "'",
                         "WASI backends only produce plain .wasm modules (output=wasm). Browser output needs the emscripten backend."});
    }
  }
  S.opt = S.backend->optimization(S.rc.strategy);

  // Dependencies, filtered to what the selected sources can actually reach.
  const auto active = activeFiles(S);
  std::set<std::string> flagSeen;
  for (const auto& [dep, files] : S.proj.depFiles) {
    bool used = false;
    for (const auto& f : files)
      if (active.count(f)) used = true;
    if (!used) continue;
    S.activeDeps[dep] = S.proj.depHeaders[dep];
    const DepSupport sup = S.backend->dependency(dep);
    const std::string hdrs = join(std::vector<std::string>(S.proj.depHeaders[dep].begin(), S.proj.depHeaders[dep].end()), ", ");
    if (sup.supported) {
      for (const auto& f : sup.flags)
        if (flagSeen.insert(f).second) S.depFlags.push_back(f);
      if (sup.flags.empty() && dep != "emscripten") S.diags.push_back({Level::Info, "I-DEP", dep + " (" + hdrs + "): " + sup.note, ""});
    } else {
      S.diags.push_back({Level::Warning, "W-DEP", "dependency '" + dep + "' (" + hdrs + ") is not supported by " + S.backend->id() + ": " + sup.note,
                         "If the include sits inside an #ifdef for another platform this is harmless; otherwise the build will fail at compile or link time."});
    }
  }
  {  // Config headers that the project's own build system would normally generate.
    TemplateContext tc;
    tc.projectName = S.proj.name;
    tc.activeFiles = active;
    for (const auto& v : so.vars) {
      std::string n, val;
      if (parseVarAssignment(v, n, val)) tc.user[n] = {true, val, "user", ""};
    }
    for (const auto& [dep, hdrs] : S.activeDeps) tc.depSupported[dep] = S.backend->dependency(dep).supported;
    S.generated = planGeneratedHeaders(S.proj, tc, S.diags);
    for (const auto& g : S.generated) {
      S.proj.missing.erase(g.include);
      std::map<std::string, int> byOrigin;
      for (const auto& u : g.uses) byOrigin[u.var.origin]++;
      std::string summary;
      for (const auto& [o, n] : byOrigin) summary += (summary.empty() ? "" : ", ") + std::to_string(n) + " " + o;
      S.diags.push_back({Level::Info, "I-GENERATED", "\"" + g.include + "\" generated from " + g.templateRel + " (" + summary + ")",
                         "Nothing in your sources was modified. See every value with `wasmfrg gen-config`; override with --var NAME=VALUE."});
    }
  }
  for (const auto& [inc, files] : S.proj.missing) {
    std::vector<std::string> act;
    for (const auto& f : files)
      if (active.count(f)) act.push_back(f);
    if (act.empty()) continue;
    std::string hint = "If this header is generated by the project's own build system (CMake/autotools), WasmForge does not run those scripts automatically; generate it once and add it to the project, or supply it via --flag -I<dir>.";
    S.diags.push_back({Level::Warning, "W-MISSING-INCLUDE", "\"" + inc + "\" not found (included by " + std::to_string(act.size()) + " file(s), e.g. " + act.front() + ")", hint});
  }
  if (!S.proj.unknownExternal.empty()) {
    std::vector<std::string> u(S.proj.unknownExternal.begin(), S.proj.unknownExternal.end());
    if (u.size() > 8) u.resize(8);
    S.diags.push_back({Level::Info, "I-EXTERNAL", "unrecognised external headers: " + join(u, ", "), "They may belong to libraries that need a wasm build."});
  }

  S.assets = planAssets(S.proj, S.rc.assets, so.forceAssets);
  for (const auto& a : S.assets) {
    if (a.kind == AssetKind::SharedLib || a.kind == AssetKind::NativeBinary)
      S.diags.push_back({Level::Warning, "W-NATIVE-BINARY", a.rel + ": " + a.reason, ""});
    else if (a.kind == AssetKind::StaticLib && a.action == AssetAction::Skip)
      S.diags.push_back({Level::Warning, "W-STATIC-LIB", a.rel + ": " + a.reason, ""});
    if (!a.note.empty() && (a.action != AssetAction::Skip || a.kind == AssetKind::Wad))
      S.diags.push_back({Level::Warning, "W-ASSET", a.rel + ": " + a.note, ""});
  }
  return !S.hasError();
}

void printSession(std::ostream& os, const Session& S, bool explain) {
  const auto& P = S.proj;
  os << "Project   " << pathStr(S.root) << " (name: " << P.name << ")\n";
  os << "  build systems : " << (P.buildSystems.empty() ? "none detected" : join(P.buildSystems, ", ")) << "\n";
  os << "  languages     : " << (P.hasC ? "C " : "") << (P.hasCxx ? "C++" : "") << (!P.hasC && !P.hasCxx ? "none" : "") << "\n";
  std::vector<std::string> mains;
  for (const auto& s : P.sources)
    if (s.hasMain) mains.push_back(s.rel);
  os << "  sources       : " << P.sources.size() << ", headers: " << P.headers.size() << ", other files: " << P.otherFiles.size() << " (" << P.filesScanned << " files scanned)\n";
  os << "  entry points  : " << (mains.empty() ? "none (library)" : join(mains, ", ")) << "\n";
  std::vector<std::string> dirs(P.usedIncludeDirs.begin(), P.usedIncludeDirs.end());
  os << "  include dirs  : " << (dirs.empty() ? "(none needed)" : join(dirs, ", ")) << "\n";
  os << "Dependencies\n";
  if (S.activeDeps.empty()) os << "  (none beyond the C/C++ standard library)\n";
  for (const auto& [dep, hdrs] : S.activeDeps) {
    const DepSupport sup = S.backend->dependency(dep);
    os << "  " << std::left << std::setw(14) << dep << (sup.supported ? "ok      " : "MISSING ") << (sup.flags.empty() ? "" : join(sup.flags, " ") + "  ") << "- " << sup.note << "\n";
  }
  if (!S.generated.empty()) {
    os << "Generated headers\n";
    for (const auto& g : S.generated) os << "  " << g.include << " <- " << g.templateRel << "\n";
  }
  os << "Hardware\n" << describeHardware(S.hw);
  const auto& pr = S.backend->probe();
  os << "Backend   " << S.backend->displayName() << ": " << (pr.available ? "available (" + pr.version + ")" : "NOT AVAILABLE - " + pr.note) << "\n";
  os << "Decisions" << (explain ? " (why)" : "") << "\n" << describeDecisions(S.rc);
  os << "Plan      optimisation: " << S.opt.summary << "\n";
  if (!S.depFlags.empty()) os << "          dependency flags: " << join(S.depFlags, " ") << "\n";
  std::size_t emb = 0, ext = 0, skip = 0, lnk = 0;
  for (const auto& a : S.assets) (a.action == AssetAction::Embed ? emb : a.action == AssetAction::External ? ext : a.action == AssetAction::Link ? lnk : skip)++;
  os << "          assets: " << ext << " external, " << emb << " embedded, " << lnk << " linked, " << skip << " skipped\n";
  if (!S.diags.empty()) {
    os << "Diagnostics\n";
    printDiagnostics(os, S.diags);
  }
}

void printAssetReport(std::ostream& os, const Session& S, bool decisions) {
  os << std::left << std::setw(40) << "file" << std::setw(14) << "kind" << std::setw(10) << "size";
  os << (decisions ? "decision\n" : "detected by\n");
  std::uint64_t ext = 0, emb = 0;
  for (const auto& a : S.assets) {
    std::string name = a.rel.size() > 38 ? "..." + a.rel.substr(a.rel.size() - 35) : a.rel;
    os << std::left << std::setw(40) << name << std::setw(14) << toString(a.kind) << std::setw(10) << humanBytes(a.size);
    if (decisions) os << toString(a.action) << " - " << a.reason << "\n";
    else os << a.detectedBy << (a.signature.empty() ? "" : " (" + a.signature + ")") << (a.archiveContent.empty() ? "" : ", contents: " + a.archiveContent) << "\n";
    if (!a.note.empty()) os << "    note: " << a.note << "\n";
    if (a.action == AssetAction::External) ext += a.size;
    if (a.action == AssetAction::Embed) emb += a.size;
  }
  if (S.assets.empty()) os << "(no non-source files found)\n";
  if (decisions) os << "packaged: " << humanBytes(ext) << " external, " << humanBytes(emb) << " embedded. Original bytes are never modified (TRANSFORM is not implemented).\n";
}

void printOptimizationPlan(std::ostream& os, const Session& S) {
  os << "Strategy   " << toString(S.rc.strategy) << "  (" << S.opt.summary << ")\n";
  os << "Compile    " << join(S.opt.compile, " ") << "\n";
  os << "Link       " << join(S.opt.link, " ") << "\n\n";
  os << "What is done and by whom (no overstatement):\n"
        "  constant folding / propagation, dead-code elimination, redundant-instruction elimination,\n"
        "  control-flow simplification, inlining ........ performed by the backend's LLVM pipeline and Binaryen (wasm-opt);\n"
        "                                                 WasmForge selects the strategy and passes the flags.\n"
        "  link-time dead-code removal ................. wasm-ld / Emscripten metadce, enabled by the flags above.\n"
        "  build-time optimisation ..................... WasmForge: content-addressed object cache, memory-aware scheduler.\n"
        "  own IR-level optimiser ...................... NOT implemented. WasmForge does not transform code itself, so\n"
        "                                                 there is no risk of it changing program semantics.\n";
  if (auto w = findInPath("wasm-opt")) os << "Binaryen   wasm-opt found at " << pathStr(*w) << "\n";
  else os << "Binaryen   wasm-opt not in PATH (Emscripten ships its own copy)\n";
}

void printGraphDot(std::ostream& os, const Session& S) {
  os << "digraph wasmforge {\n  rankdir=LR;\n  node [fontname=\"monospace\"];\n";
  for (const auto& s : S.proj.sources) os << "  \"" << s.rel << "\" [shape=box" << (s.hasMain ? ",style=filled,fillcolor=lightgoldenrod" : "") << "];\n";
  for (const auto& [file, hs] : S.proj.includes)
    for (const auto& h : hs) os << "  \"" << file << "\" -> \"" << h << "\";\n";
  for (const auto& [dep, files] : S.proj.depFiles)
    for (const auto& f : files) os << "  \"" << f << "\" -> \"dep:" << dep << "\" [style=dashed];\n";
  os << "}\n";
}

CompileJob makeCompileJob(const Session& S, const SourceFile& sf) {
  CompileJob j;
  j.source = sf.rel;
  j.cxx = sf.cxx;
  j.includeDirs.assign(S.proj.usedIncludeDirs.begin(), S.proj.usedIncludeDirs.end());
  if (!S.generated.empty()) j.includeDirs.push_back(".wasmforge/gen/include");
  j.defines = S.opts.defines;
  j.flags = S.opt.compile;
  j.flags.insert(j.flags.end(), S.depFlags.begin(), S.depFlags.end());
  j.flags.insert(j.flags.end(), S.opts.extraFlags.begin(), S.opts.extraFlags.end());
  return j;
}

bool writeGeneratedHeaders(const Session& S, std::string& err) {
  const fs::path base = S.root / ".wasmforge" / "gen" / "include";
  for (const auto& g : S.generated) {
    const fs::path dst = S.root / fromUtf8(g.outRel);
    if (!isInside(base, dst)) { err = "refusing to write outside the generated-include directory: " + g.include; return false; }
    if (!writeFileAtomic(dst, g.content)) { err = "cannot write " + pathStr(dst); return false; }
  }
  return true;
}

void printGeneratedHeaders(std::ostream& os, const Session& S, bool verbose) {
  if (S.generated.empty()) {
    os << "No generated headers: no missing #include has a matching template (*.h.in, *.h.cin, *.h.cmake).\n";
    return;
  }
  for (const auto& g : S.generated) {
    os << g.include << "  <-  " << g.templateRel << "  (included by " << g.includers.size() << " file(s); written to " << g.outRel << ")\n";
    std::size_t shown = 0;
    for (const auto& u : g.uses) {
      if (u.kind == "subst" && !verbose) continue;
      if (!verbose && shown >= 60) { os << "    ... (use --verbose for all)\n"; break; }
      ++shown;
      os << "    [" << (u.enabled ? "on " : "off") << "] " << std::left << std::setw(28) << u.name << std::setw(14) << u.kind << " = "
         << std::setw(14) << (u.var.defined ? (u.var.value.empty() ? "\"\"" : u.var.value) : "(undefined)") << " (" << u.var.origin << ")" << (u.var.note.empty() ? "" : "  " + u.var.note) << "\n";
    }
  }
  os << "Origins: user = --var, derived = from detected dependencies, guess = assumed for the wasm libc, placeholder = needs your value, unset = not probed.\n";
}

namespace {
struct Tarjan {
  const std::map<std::string, std::vector<std::string>>& g;
  std::map<std::string, int> idx, low;
  std::set<std::string> on;
  std::vector<std::string> st;
  std::vector<std::vector<std::string>> sccs;
  int counter = 0;
  explicit Tarjan(const std::map<std::string, std::vector<std::string>>& graph) : g(graph) {}
  void dfs(const std::string& v) {
    idx[v] = low[v] = counter++;
    st.push_back(v);
    on.insert(v);
    auto it = g.find(v);
    if (it != g.end())
      for (const auto& w : it->second) {
        if (!idx.count(w)) { dfs(w); low[v] = std::min(low[v], low[w]); }
        else if (on.count(w)) low[v] = std::min(low[v], idx[w]);
      }
    if (low[v] == idx[v]) {
      std::vector<std::string> comp;
      std::string w;
      do { w = st.back(); st.pop_back(); on.erase(w); comp.push_back(w); } while (w != v);
      if (comp.size() > 1) sccs.push_back(comp);
    }
  }
};
}  // namespace

void printAnalysis(std::ostream& os, const Session& S, const std::string& touch) {
  const auto& P = S.proj;
  const std::size_t total = P.sources.size();
  std::map<std::string, std::set<std::string>> dependents;  // header -> sources that (transitively) include it
  std::vector<std::pair<std::string, std::size_t>> weight;   // source -> number of headers it pulls in
  for (const auto& s : P.sources) {
    const auto c = transitiveHeaders(P, s.rel);
    weight.emplace_back(s.rel, c.size());
    for (const auto& h : c) dependents[h].insert(s.rel);
  }
  auto pct = [&](std::size_t n) { return total ? static_cast<int>(n * 100 / total) : 0; };
  os << "Build graph: " << total << " source(s), " << P.headers.size() << " header(s)\n";

  std::vector<std::pair<std::string, std::size_t>> radius;
  for (const auto& [h, d] : dependents) radius.emplace_back(h, d.size());
  std::sort(radius.begin(), radius.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  os << "\nRebuild blast radius (editing this header recompiles this many units)\n";
  if (radius.empty()) os << "  (no project headers are included by the selected sources)\n";
  for (std::size_t i = 0; i < radius.size() && i < 10; ++i)
    os << "  " << std::left << std::setw(44) << radius[i].first << radius[i].second << " of " << total << " units (" << pct(radius[i].second) << "%)\n";
  if (!radius.empty() && total >= 5 && pct(radius[0].second) >= 50)
    os << "  hint: " << radius[0].first << " triggers a near-full rebuild; splitting it or forward-declaring would speed up incremental builds.\n";

  std::sort(weight.begin(), weight.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  os << "\nHeaviest sources (most headers pulled in)\n";
  for (std::size_t i = 0; i < weight.size() && i < 5; ++i) os << "  " << std::left << std::setw(44) << weight[i].first << weight[i].second << " header(s)\n";

  Tarjan t(P.includes);
  for (const auto& [f, hs] : P.includes)
    if (!t.idx.count(f)) t.dfs(f);
  os << "\nInclude cycles: " << (t.sccs.empty() ? "none" : std::to_string(t.sccs.size())) << "\n";
  for (std::size_t i = 0; i < t.sccs.size() && i < 5; ++i) {
    auto c = t.sccs[i];
    std::sort(c.begin(), c.end());
    os << "  " << join(c, " <-> ") << "\n";
  }

  std::set<std::string> included;
  for (const auto& [f, hs] : P.includes) included.insert(hs.begin(), hs.end());
  std::vector<std::string> orphans;
  for (const auto& h : P.headers)
    if (!included.count(h)) orphans.push_back(h);
  os << "\nHeaders nobody includes: " << orphans.size() << (orphans.empty() ? "" : "  (may be public API or dead code)") << "\n";
  for (std::size_t i = 0; i < orphans.size() && i < 8; ++i) os << "  " << orphans[i] << "\n";

  if (!touch.empty()) {
    const std::string key = fs::path(touch).lexically_normal().generic_string();
    os << "\nIf you edit " << key << ":\n";
    bool isSource = false;
    for (const auto& s : P.sources) isSource = isSource || s.rel == key;
    if (isSource) os << "  recompiles 1 of " << total << " unit(s) (just itself)\n";
    else if (dependents.count(key)) {
      const auto& d = dependents.at(key);
      os << "  recompiles " << d.size() << " of " << total << " unit(s) (" << pct(d.size()) << "%):\n";
      std::size_t n = 0;
      for (const auto& s : d) { if (++n > 25) { os << "  ...\n"; break; } os << "    " << s << "\n"; }
    } else if (P.headers.count(key)) os << "  no selected source includes it: nothing is recompiled\n";
    else os << "  '" << key << "' is not a known source or header of this project\n";
  }
}

bool writeCompileDb(const Session& S, const fs::path& out, std::string& err) {
  std::string gerr;
  if (!writeGeneratedHeaders(S, gerr)) { err = gerr; return false; }
  std::ostringstream o;
  o << "[\n";
  for (std::size_t i = 0; i < S.proj.sources.size(); ++i) {
    CompileJob j = makeCompileJob(S, S.proj.sources[i]);
    j.object = ".wasmforge/obj/" + j.source + ".o";
    const auto argv = S.backend->compileArgv(j);
    o << "  {\"directory\": \"" << jsonEscape(pathStr(S.root)) << "\", \"file\": \"" << jsonEscape(j.source) << "\", \"arguments\": [";
    for (std::size_t k = 0; k < argv.size(); ++k) o << (k ? ", " : "") << "\"" << jsonEscape(argv[k]) << "\"";
    o << "]}" << (i + 1 < S.proj.sources.size() ? "," : "") << "\n";
  }
  o << "]\n";
  if (!writeFileAtomic(out, o.str())) { err = "cannot write " + pathStr(out); return false; }
  return true;
}

// ================================================================================================
namespace {
struct Unit {
  const SourceFile* src = nullptr;
  std::string key;
  fs::path obj, meta;
  CompileJob job;
  enum { Pending, Cached, Compiled, Failed } status = Pending;
  std::string output;
  double seconds = 0;
  std::uint64_t rss = 0;
};

bool cacheHit(const fs::path& obj, const fs::path& meta) {
  std::error_code ec;
  if (!fs::exists(obj, ec)) return false;
  const auto sz = fs::file_size(obj, ec);
  if (ec || sz == 0) return false;
  auto m = readFile(meta, 64);
  return m && trim(*m) == "size=" + std::to_string(sz);
}

bool memoryAllowsAnotherJob(unsigned perJobMb) {
  const MemoryInfo m = queryMemory();
  if (!m.ok || m.availableBytes == 0) return true;
  return (m.availableBytes >> 20) > static_cast<std::uint64_t>(perJobMb) + 256;
}

std::string tail(const std::string& s, int lines) {
  auto parts = split(s, '\n');
  std::string o;
  const std::size_t start = parts.size() > static_cast<std::size_t>(lines) ? parts.size() - lines : 0;
  for (std::size_t i = start; i < parts.size(); ++i) o += "    | " + parts[i] + "\n";
  return o;
}

void pruneCache(const fs::path& dir, std::uint64_t capMb, const std::set<std::string>& keep) {
  struct E { fs::path p; fs::file_time_type t; std::uint64_t sz; };
  std::vector<E> files;
  std::uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const auto sz = static_cast<std::uint64_t>(it->file_size(ec));
    total += sz;
    if (it->path().extension() == ".o") files.push_back({it->path(), it->last_write_time(ec), sz});
  }
  if (total <= (capMb << 20)) return;
  std::sort(files.begin(), files.end(), [](const E& a, const E& b) { return a.t < b.t; });
  for (const auto& f : files) {
    if (total <= (capMb << 20)) break;
    if (keep.count(f.p.stem().string())) continue;
    fs::remove(f.p, ec);
    fs::path meta = f.p;
    meta.replace_extension(".meta");
    fs::remove(meta, ec);
    total -= std::min(total, f.sz);
  }
}

std::string vfsLoader(const std::vector<const AssetInfo*>& ext) {
  std::ostringstream o;
  o << "// Generated by WasmForge. Mounts external assets (original bytes) into the Emscripten virtual\n"
       "// filesystem before main() runs. In a browser they are fetched relative to the page; under Node\n"
       "// they are read from disk relative to this script, so the same build can be smoke-tested headless.\n"
       "(function () {\n  var assets = [\n";
  for (std::size_t i = 0; i < ext.size(); ++i)
    o << "    {\"url\": \"assets/" << jsonEscape(ext[i]->rel) << "\", \"path\": \"/" << jsonEscape(ext[i]->rel) << "\"}" << (i + 1 < ext.size() ? "," : "") << "\n";
  o << "  ];\n"
       "  var isNode = typeof process === 'object' && process.versions && process.versions.node && typeof require === 'function' && typeof __dirname === 'string';\n"
       "  function load(a) {\n"
       "    if (isNode) {\n"
       "      return new Promise(function (resolve, reject) {\n"
       "        require('fs').readFile(require('path').join(__dirname, a.url), function (e, b) { e ? reject(e) : resolve(new Uint8Array(b)); });\n"
       "      });\n"
       "    }\n"
       "    return fetch(a.url).then(function (r) {\n"
       "      if (!r.ok) throw new Error('HTTP ' + r.status);\n"
       "      return r.arrayBuffer();\n"
       "    }).then(function (b) { return new Uint8Array(b); });\n"
       "  }\n"
       "  Module['preRun'] = Module['preRun'] || [];\n"
       "  Module['preRun'].push(function () {\n"
       "    assets.forEach(function (a) {\n"
       "      var dep = 'wasmfrg:' + a.path;\n"
       "      addRunDependency(dep);\n"
       "      load(a).then(function (bytes) {\n"
       "        var dir = a.path.substring(0, a.path.lastIndexOf('/'));\n"
       "        if (dir) FS.mkdirTree(dir);\n"
       "        FS.writeFile(a.path, bytes);\n"
       "        removeRunDependency(dep);\n"
       "      }).catch(function (e) {\n"
       "        err('wasmfrg: could not load ' + a.url + ': ' + e);\n"
       "      });\n"
       "    });\n"
       "  });\n"
       "})();\n";
  return o.str();
}
}  // namespace

BuildReport runBuild(const BuildOptions& bo, std::ostream& out) {
  BuildReport R;
  const auto tAll = Clock::now();
  Session S;
  prepareSession(bo.session, S);

  if (bo.explain || bo.dryRun) {
    out << "Hardware\n" << describeHardware(S.hw) << "Decisions\n" << describeDecisions(S.rc);
  }
  // Fatal errors: everything except a missing backend during --dry-run.
  bool fatal = false;
  int code = 1;
  for (const auto& d : S.diags) {
    if (d.level != Level::Error) continue;
    if (d.code == "E-NOBACKEND" && bo.dryRun) continue;
    fatal = true;
    if (d.code == "E-CONFIG") code = 4;
    else if (d.code == "E-NOBACKEND") code = std::max(code == 1 ? 3 : code, 3);
  }
  printDiagnostics(out, S.diags);
  if (fatal) {
    R.exitCode = code;
    R.totalSeconds = since(tAll);
    return R;
  }

  const auto& proj = S.proj;
  const fs::path cacheDir = S.root / ".wasmforge" / "cache";
  const fs::path genDir = S.root / ".wasmforge" / "gen";

  // ---- plan compile units + cache keys --------------------------------------------------------
  std::map<std::string, std::uint64_t> fileHash;
  auto hashOf = [&](const std::string& rel, std::uint64_t& h) {
    auto it = fileHash.find(rel);
    if (it != fileHash.end()) { h = it->second; return true; }
    if (!hashFile(S.root / fromUtf8(rel), h)) return false;
    fileHash[rel] = h;
    return true;
  };
  std::vector<Unit> units(proj.sources.size());
  bool anyCxx = false;
  for (std::size_t i = 0; i < units.size(); ++i) {
    Unit& u = units[i];
    u.src = &proj.sources[i];
    anyCxx = anyCxx || u.src->cxx;
    u.job = makeCompileJob(S, *u.src);
    std::uint64_t h = fnv1a(S.backend->id());
    auto mix = [&](const std::string& s) { h = fnv1a(s, h); h = fnv1a("\x1f", 1, h); };
    mix(S.backend->probe().version);
    mix(u.job.cxx ? "c++" : "c");
    mix(u.src->rel);
    for (const auto& f : u.job.flags) mix(f);
    for (const auto& d : u.job.defines) mix("D" + d);
    for (const auto& d : u.job.includeDirs) mix("I" + d);
    std::uint64_t fh = 0;
    if (!hashOf(u.src->rel, fh)) {
      out << "error[E-READ]: cannot read " << u.src->rel << "\n";
      R.exitCode = 1;
      return R;
    }
    mix(hex64(fh));
    std::set<std::string> closure = transitiveHeaders(proj, u.src->rel);
    for (const auto& hdr : closure) {
      if (!hashOf(hdr, fh)) { out << "error[E-READ]: cannot read header " << hdr << "\n"; R.exitCode = 1; return R; }
      mix(hdr);
      mix(hex64(fh));
    }
    closure.insert(u.src->rel);
    for (const auto& g : S.generated) {  // a regenerated config.h must invalidate exactly the units that include it
      bool hit = false;
      for (const auto& f : g.includers) hit = hit || closure.count(f) > 0;
      if (hit) mix("gen:" + g.include + hex64(g.hash));
    }
    u.key = hex64(h);
    u.obj = cacheDir / (u.key + ".o");
    u.meta = cacheDir / (u.key + ".meta");
    u.job.object = pathStr(cacheDir / (u.key + ".tmp.o"));
  }
  R.units = units.size();

  // ---- link plan ------------------------------------------------------------------------------
  LinkJob link;
  link.kind = S.rc.output;
  link.cxx = anyCxx;
  link.html = S.rc.html;
  link.noEntry = !proj.hasEntry() || std::none_of(proj.sources.begin(), proj.sources.end(), [](const SourceFile& f) { return f.hasMain; });
  link.flags = S.opt.link;
  link.flags.insert(link.flags.end(), S.depFlags.begin(), S.depFlags.end());
  link.flags.insert(link.flags.end(), bo.session.extraFlags.begin(), bo.session.extraFlags.end());
  for (const auto& u : units) link.inputs.push_back(pathStr(u.obj));
  std::vector<const AssetInfo*> externals;
  const bool canShipAssets = S.rc.output != OutputKind::WasmOnly;
  for (const auto& a : S.assets) {
    if (a.action == AssetAction::Link) link.inputs.push_back(a.rel);
    else if (a.action == AssetAction::Embed && canShipAssets) link.embedFiles.push_back(a.rel + "@/" + a.rel);
    else if (a.action == AssetAction::External && canShipAssets) externals.push_back(&a);
  }
  std::size_t droppedAssets = 0;
  for (const auto& a : S.assets)
    if (!canShipAssets && (a.action == AssetAction::Embed || a.action == AssetAction::External)) ++droppedAssets;
  if (droppedAssets) out << "warning[W-ASSET-TARGET]: output=wasm cannot carry assets; " << droppedAssets << " referenced asset(s) were not packaged\n";
  const fs::path vfsFile = genDir / "vfs-preload.js";
  const bool useVfs = !externals.empty() && S.backend->id() == "emscripten";
  if (useVfs) {
    link.preJs.push_back(pathStr(vfsFile));
    link.needsFilesystem = true;
  }
  link.output = pathStr(S.outDir / S.backend->primaryOutput(S.rc.name, S.rc.output, S.rc.html));

  if (bo.dryRun) {
    out << "\nDry run: " << units.size() << " compile unit(s), " << S.rc.workers << " worker(s). Nothing was executed or written.\n";
    for (const auto& g : S.generated) out << "  would generate " << g.outRel << " from " << g.templateRel << "\n";
    const std::size_t show = bo.verbose ? units.size() : std::min<std::size_t>(3, units.size());
    for (std::size_t i = 0; i < show; ++i) out << "  compile: " << displayArgv(S.backend->compileArgv(units[i].job)) << "\n";
    if (show < units.size()) out << "  ... " << units.size() - show << " more (use --verbose to list all)\n";
    out << "  link:    " << displayArgv(S.backend->linkArgv(link)) << "\n";
    R.ok = true;
    R.totalSeconds = since(tAll);
    return R;
  }

  std::error_code ec;
  fs::create_directories(cacheDir, ec);
  {
    std::string gerr;
    if (!writeGeneratedHeaders(S, gerr)) { out << "error[E-IO]: " << gerr << "\n"; R.exitCode = 1; return R; }
  }
  if (ec) { out << "error[E-IO]: cannot create " << pathStr(cacheDir) << ": " << ec.message() << "\n"; R.exitCode = 1; return R; }

  // ---- compile phase --------------------------------------------------------------------------
  std::size_t pending = 0;
  for (auto& u : units) {
    if (!bo.force && cacheHit(u.obj, u.meta)) {
      u.status = Unit::Cached;
      fs::last_write_time(u.obj, fs::file_time_type::clock::now(), ec);  // LRU touch
    } else {
      ++pending;
    }
  }
  out << "\nCompiling " << pending << " of " << units.size() << " unit(s) (" << units.size() - pending << " from cache) with " << S.rc.workers
      << " worker(s), memory budget " << S.rc.memoryMb << " MB\n";
  const auto tCompile = Clock::now();
  std::atomic<std::size_t> next{0};
  std::atomic<bool> abortAll{false};
  std::atomic<int> active{0};
  std::mutex logMu;
  auto worker = [&]() {
    for (;;) {
      const std::size_t i = next.fetch_add(1);
      if (i >= units.size() || abortAll.load()) break;
      Unit& u = units[i];
      if (u.status == Unit::Cached) continue;
      while (!abortAll.load() && active.load() > 0 && !memoryAllowsAnotherJob(S.rc.perJobMb)) std::this_thread::sleep_for(std::chrono::milliseconds(150));
      active.fetch_add(1);
      const auto argv = S.backend->compileArgv(u.job);
      if (bo.verbose) {
        std::lock_guard<std::mutex> lk(logMu);
        out << "  [" << i + 1 << "/" << units.size() << "] " << u.src->rel << "\n";
      }
      const ProcessResult pr = runProcess(argv, S.root, true);
      active.fetch_sub(1);
      u.seconds = pr.seconds;
      u.rss = pr.peakRssKb;
      const fs::path tmp = fromUtf8(u.job.object);
      std::error_code e2;
      if (pr.started && pr.exitCode == 0 && fs::exists(tmp, e2) && fs::file_size(tmp, e2) > 0) {
        const auto sz = fs::file_size(tmp, e2);
        fs::rename(tmp, u.obj, e2);
        if (!e2) writeFileAtomic(u.meta, "size=" + std::to_string(sz) + "\n");
        u.status = e2 ? Unit::Failed : Unit::Compiled;
        if (e2) u.output = "cannot move object into cache: " + e2.message();
      } else {
        fs::remove(tmp, e2);
        u.status = Unit::Failed;
        u.output = pr.started ? pr.output : pr.error;
        if (pr.started && !pr.error.empty()) u.output += "\n" + pr.error;
        if (!bo.keepGoing) abortAll = true;
      }
    }
  };
  if (pending > 0) {
    const unsigned n = std::max(1u, std::min<unsigned>(S.rc.workers, static_cast<unsigned>(pending)));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }
  R.compileSeconds = since(tCompile);
  std::set<std::string> keys;
  for (const auto& u : units) {
    keys.insert(u.key);
    R.peakChildRssKb = std::max(R.peakChildRssKb, u.rss);
    switch (u.status) {
      case Unit::Cached: ++R.cached; break;
      case Unit::Compiled: ++R.compiled; break;
      case Unit::Failed: ++R.failed; break;
      default: break;
    }
  }
  for (const auto& u : units)
    if (u.status == Unit::Failed) out << "\nerror[E-COMPILE]: " << u.src->rel << " failed\n" << tail(u.output, 40);
  if (R.failed > 0) {
    out << "\nBuild FAILED: " << R.failed << " unit(s) did not compile; " << R.compiled << " compiled, " << R.cached << " cached.\n"
        << "    hint: successfully compiled units are cached, so a re-run only repeats the failing ones.\n";
    pruneCache(cacheDir, S.rc.cacheMb, keys);
    R.exitCode = 1;
    R.totalSeconds = since(tAll);
    return R;
  }
  const std::size_t notBuilt = std::count_if(units.begin(), units.end(), [](const Unit& u) { return u.status == Unit::Pending; });
  if (notBuilt) { R.exitCode = 1; R.totalSeconds = since(tAll); return R; }

  // ---- packaging: assets, loader, link --------------------------------------------------------
  fs::create_directories(S.outDir, ec);
  if (useVfs && !writeFileAtomic(vfsFile, vfsLoader(externals))) { out << "error[E-IO]: cannot write " << pathStr(vfsFile) << "\n"; R.exitCode = 1; return R; }
  struct Shipped { std::string rel; std::uint64_t size; AssetKind kind; };
  std::vector<Shipped> shipped;
  for (const auto* a : externals) {
    const fs::path src = S.root / fromUtf8(a->rel), dst = S.outDir / "assets" / fromUtf8(a->rel);
    if (!isInside(S.outDir, dst)) { out << "error[E-PATH]: refusing to write outside the output directory: " << a->rel << "\n"; R.exitCode = 1; return R; }
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { out << "error[E-IO]: cannot copy asset " << a->rel << ": " << ec.message() << "\n"; R.exitCode = 1; return R; }
    shipped.push_back({a->rel, a->size, a->kind});
  }

  const auto tLink = Clock::now();
  const ProcessResult lr = runProcess(S.backend->linkArgv(link), S.root, true);
  R.linkSeconds = since(tLink);
  R.peakChildRssKb = std::max(R.peakChildRssKb, lr.peakRssKb);
  if (!lr.started || lr.exitCode != 0) {
    out << "\nerror[E-LINK]: link step failed\n" << tail(lr.started ? lr.output : lr.error, 40);
    out << "    hint: unresolved symbols usually mean a library or a platform-specific source file is missing (see warnings above); use --exclude / --flag.\n";
    R.exitCode = 1;
    R.totalSeconds = since(tAll);
    return R;
  }

  // ---- validation (structural only) -----------------------------------------------------------
  std::vector<std::string> arts = S.backend->artifacts(S.rc.name, S.rc.output, S.rc.html);
  std::vector<std::pair<std::string, std::uint64_t>> artSizes;
  std::string wasmReport;
  for (const auto& a : arts) {
    const fs::path p = S.outDir / fromUtf8(a);
    std::error_code e3;
    const auto sz = fs::exists(p, e3) ? fs::file_size(p, e3) : 0;
    std::string why;
    if (sz == 0) why = "missing or empty";
    else if (endsWith(a, ".wasm") && bo.budgetBytes && sz > bo.budgetBytes)
      why = "size budget exceeded: " + humanBytes(sz) + " > " + humanBytes(bo.budgetBytes) + " (see `wasmfrg wasm-info` for what dominates, or try --strategy compact)";
    else if (endsWith(a, ".wasm")) {
      const WasmInfo wi = parseWasm(readFile(p, 1u << 30).value_or(""));
      const auto wd = wasmDiagnostics(wi, S.rc.output != OutputKind::WasmOnly);
      printDiagnostics(out, wd);
      for (const auto& x : wd)
        if (x.level == Level::Error) why = x.message;
      if (wi.valid) wasmReport += describeWasm(wi);
    }
    if (!why.empty()) {
      out << "error[E-VALIDATE]: output " << a << ": " << why << "\n";
      R.exitCode = 1;
      R.totalSeconds = since(tAll);
      return R;
    }
    artSizes.emplace_back(a, static_cast<std::uint64_t>(sz));
    R.artifacts.push_back(pathStr(p));
  }
  if (S.rc.output != OutputKind::WasmOnly) {
    std::ostringstream m;
    m << "{\n  \"name\": \"" << jsonEscape(S.rc.name) << "\",\n  \"generator\": \"wasmfrg 0.1.0\",\n  \"backend\": \"" << jsonEscape(S.backend->id() + " " + S.backend->probe().version)
      << "\",\n  \"target\": \"" << toString(S.rc.output) << "\",\n  \"entry\": \"" << jsonEscape(S.backend->primaryOutput(S.rc.name, S.rc.output, S.rc.html)) << "\",\n  \"files\": [\n";
    for (std::size_t i = 0; i < artSizes.size(); ++i) {
      std::uint64_t h = 0;
      hashFile(S.outDir / fromUtf8(artSizes[i].first), h);
      m << "    {\"path\": \"" << jsonEscape(artSizes[i].first) << "\", \"bytes\": " << artSizes[i].second << ", \"fnv1a64\": \"" << hex64(h) << "\"}" << (i + 1 < artSizes.size() ? "," : "") << "\n";
    }
    m << "  ],\n  \"assets\": [\n";
    std::vector<std::string> rows;
    for (const auto& s : shipped)
      rows.push_back("    {\"source\": \"" + jsonEscape(s.rel) + "\", \"path\": \"assets/" + jsonEscape(s.rel) + "\", \"mount\": \"/" + jsonEscape(s.rel) + "\", \"mode\": \"external\", \"kind\": \"" + toString(s.kind) + "\", \"bytes\": " + std::to_string(s.size) + "}");
    for (const auto& a : S.assets)
      if (a.action == AssetAction::Embed)
        rows.push_back("    {\"source\": \"" + jsonEscape(a.rel) + "\", \"mount\": \"/" + jsonEscape(a.rel) + "\", \"mode\": \"embed\", \"kind\": \"" + toString(a.kind) + "\", \"bytes\": " + std::to_string(a.size) + "}");
    for (std::size_t i = 0; i < rows.size(); ++i) m << rows[i] << (i + 1 < rows.size() ? "," : "") << "\n";
    m << "  ]\n}\n";
    if (!writeFileAtomic(S.outDir / "manifest.json", m.str())) { out << "error[E-IO]: cannot write manifest.json\n"; R.exitCode = 1; return R; }
    R.artifacts.push_back(pathStr(S.outDir / "manifest.json"));
  }
  pruneCache(cacheDir, S.rc.cacheMb, keys);

  R.ok = true;
  R.outDir = S.outDir;
  R.genDir = genDir;
  R.name = S.rc.name;
  R.totalSeconds = since(tAll);
  out << "\nBuild OK  " << R.units << " unit(s): " << R.compiled << " compiled, " << R.cached << " from cache\n";
  out << "  time      " << std::fixed << std::setprecision(2) << R.totalSeconds << " s total = compile phase " << R.compileSeconds << " s + link " << R.linkSeconds
      << " s (backend) + " << std::max(0.0, R.totalSeconds - R.compileSeconds - R.linkSeconds) << " s WasmForge (discovery, hashing, packaging)\n";
  if (R.peakChildRssKb) out << "  peak RAM  " << humanBytes(R.peakChildRssKb * 1024) << " (largest single backend process)\n";
  out << "  output    " << pathStr(S.outDir) << "\n";
  for (const auto& a : artSizes) out << "    " << std::left << std::setw(24) << a.first << humanBytes(a.second) << "\n";
  for (const auto& s : shipped) out << "    assets/" << s.rel << "  " << humanBytes(s.size) << " (unchanged)\n";
  if (!wasmReport.empty()) out << "  wasm module\n" << wasmReport;
  out << "  validated : output files exist and the .wasm binary parses (sections, imports, memory).\n"
         "  NOT verified: that the program actually runs in a browser/runtime (runtime tests are Phase 4/5).\n";
  return R;
}

bool cleanProject(const SessionOptions& so, bool cacheOnly, std::ostream& out) {
  std::error_code ec;
  fs::path root = fs::weakly_canonical(fs::absolute(so.project), ec);
  if (!fs::is_directory(root, ec)) { out << "error: not a directory: " << pathStr(so.project) << "\n"; return false; }
  std::vector<fs::path> targets;
  if (cacheOnly) targets.push_back(root / ".wasmforge" / "cache");
  else {
    targets.push_back(root / ".wasmforge");
    targets.push_back(so.outDir.empty() ? root / "dist" : fs::absolute(so.outDir));
  }
  bool ok = true;
  for (const auto& t : targets) {
    if (!isInside(root, t) || fs::weakly_canonical(t, ec) == root) {
      out << "refusing to delete " << pathStr(t) << ": it is not strictly inside the project directory\n";
      ok = false;
      continue;
    }
    if (!fs::exists(t, ec)) continue;
    const auto n = fs::remove_all(t, ec);
    if (ec) { out << "error: cannot remove " << pathStr(t) << ": " << ec.message() << "\n"; ok = false; }
    else out << "removed " << pathStr(t) << " (" << n << " entries)\n";
  }
  return ok;
}

}  // namespace wf
