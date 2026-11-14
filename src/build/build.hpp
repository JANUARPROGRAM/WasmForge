#pragma once
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "assets/assets.hpp"
#include "backend/backend.hpp"
#include "config/config.hpp"
#include "core/diag.hpp"
#include "hardware/hardware.hpp"
#include "project/project.hpp"
#include "project/templates.hpp"

namespace wf {

struct SessionOptions {
  fs::path project = ".";
  fs::path outDir;                       // default: <project>/dist
  ConfigFile cli;                        // command-line overrides (validated key/values)
  std::string entry;                     // chosen entry-point source when several main() exist
  std::vector<std::string> excludes;     // glob patterns of sources to leave out
  std::vector<std::string> forceAssets;  // glob patterns of assets to package even if unreferenced
  std::vector<std::string> defines;      // -D passed to every compile
  std::vector<std::string> extraFlags;   // passed to compile AND link, as separate argv items (never via a shell)
  std::vector<std::string> vars;         // NAME[=VALUE] for generated config headers (--var)
};

// Everything the toolchain knows about one project on one machine, before any compiler runs.
struct Session {
  SessionOptions opts;
  fs::path root, outDir;
  HardwareInfo hw;
  ConfigMap cm;
  ProjectInfo proj;
  ResolvedConfig rc;
  std::unique_ptr<Backend> backend;
  OptFlags opt;
  std::vector<std::string> depFlags;
  std::map<std::string, std::set<std::string>> activeDeps;
  std::vector<AssetInfo> assets;
  std::vector<GeneratedHeader> generated;  // config headers produced from templates (never written into the sources)
  std::vector<Diagnostic> diags;

  bool hasError(const std::string& code = "") const;
};

bool prepareSession(const SessionOptions& so, Session& s);
void printSession(std::ostream& os, const Session& s, bool explain);
void printAssetReport(std::ostream& os, const Session& s, bool decisions);
void printOptimizationPlan(std::ostream& os, const Session& s);
void printGraphDot(std::ostream& os, const Session& s);
void printGeneratedHeaders(std::ostream& os, const Session& s, bool verbose);
void printAnalysis(std::ostream& os, const Session& s, const std::string& touch);
bool writeGeneratedHeaders(const Session& s, std::string& err);
bool writeCompileDb(const Session& s, const fs::path& out, std::string& err);
CompileJob makeCompileJob(const Session& s, const SourceFile& sf);

struct BuildOptions {
  SessionOptions session;
  std::uint64_t budgetBytes = 0;  // fail when a .wasm output is larger (0 = no budget)
  bool dryRun = false, explain = false, verbose = false, force = false, keepGoing = false;
};

struct BuildReport {
  bool ok = false;
  int exitCode = 0;  // 0 ok | 1 build failure | 3 environment (no backend) | 4 configuration
  std::size_t units = 0, compiled = 0, cached = 0, failed = 0;
  double totalSeconds = 0, compileSeconds = 0, linkSeconds = 0;  // compileSeconds = wall time of the compile phase
  std::uint64_t peakChildRssKb = 0;
  std::vector<std::string> artifacts;
  fs::path outDir, genDir;
  std::string name;
};

BuildReport runBuild(const BuildOptions& bo, std::ostream& out);
bool cleanProject(const SessionOptions& so, bool cacheOnly, std::ostream& out);
}  // namespace wf
