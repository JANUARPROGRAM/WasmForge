#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/util.hpp"

namespace wf {

struct SourceFile {
  std::string rel;  // path relative to project root, forward slashes
  bool cxx = false;
  bool hasMain = false;
};

struct ProjectInfo {
  fs::path root;
  std::string name;
  std::vector<std::string> buildSystems;                     // cmake | make | autotools | meson
  bool hasC = false, hasCxx = false;
  std::vector<SourceFile> sources;                           // sorted by path
  std::set<std::string> headers;                             // project headers (relative)
  std::map<std::string, std::vector<std::string>> includes;  // file -> directly included project headers
  std::set<std::string> usedIncludeDirs;                     // dirs that were needed to resolve includes ("." = root)
  std::map<std::string, std::set<std::string>> depFiles;     // dependency id -> files that include it
  std::map<std::string, std::set<std::string>> depHeaders;   // dependency id -> header names seen
  std::map<std::string, std::set<std::string>> missing;      // unresolved "quoted" include -> files
  std::set<std::string> unknownExternal;                     // <angle> headers nobody recognises
  std::vector<std::string> otherFiles;                       // everything that is not source/header (asset candidates)
  std::set<std::string> referencedNames;                     // lower-case file names seen inside string literals
  std::vector<std::string> warnings;
  std::uint64_t filesScanned = 0;
  bool truncated = false;

  bool hasEntry() const {
    for (const auto& s : sources)
      if (s.hasMain) return true;
    return false;
  }
};

struct DiscoverOptions {
  std::size_t maxFiles = 200000;
  std::vector<fs::path> skipPaths;  // absolute directories to ignore (e.g. the output dir)
};

// Directories every scanner/watcher ignores (.git, .wasmforge, dist, node_modules, ...).
bool shouldSkipDirName(const std::string& name);

bool discoverProject(const fs::path& root, const DiscoverOptions& opt, ProjectInfo& out, std::string& err);

// All project headers reachable from `rel` (excluding itself). Cycle-safe.
std::set<std::string> transitiveHeaders(const ProjectInfo& p, const std::string& rel);

// kind: "std" | "posix" | "dep" | "unknown". For "dep" the returned string is the dependency id.
std::string classifyExternalHeader(const std::string& include, std::string& kind);
}  // namespace wf
