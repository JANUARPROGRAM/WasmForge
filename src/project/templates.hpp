#pragma once
// Generates configuration headers (config.h) from CMake/autotools templates WITHOUT running CMake or
// ./configure and without touching the original sources: output goes to .wasmforge/gen/include/.
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/diag.hpp"
#include "project/project.hpp"

namespace wf {

struct TemplateVar {
  bool defined = false;
  std::string value;
  std::string origin;  // user | derived | guess | placeholder | unset
  std::string note;
};
using TemplateVars = std::map<std::string, TemplateVar>;
using VarResolver = std::function<TemplateVar(const std::string&)>;

struct VarUse {
  std::string name, kind;  // kind: cmakedefine | cmakedefine01 | undef | subst
  TemplateVar var;
  bool enabled = false;    // what the generated header ended up doing with it
};

struct GeneratedHeader {
  std::string include;      // exactly as written in the #include directive
  std::string templateRel;  // template it was generated from
  std::string outRel;       // relative to the project root
  std::string content;
  std::vector<VarUse> uses;
  std::set<std::string> includers;
  std::uint64_t hash = 0;
};

struct TemplateContext {
  std::string projectName;
  TemplateVars user;                    // --var NAME[=VALUE]
  std::map<std::string, bool> depSupported;  // dependency id -> the backend can supply it
  std::set<std::string> activeFiles;    // only generate for headers needed by selected sources
};

bool isTruthy(const std::string& v);
// "src/config.h.cin" -> "config.h"; "" when the file is not a header template.
std::string templateTargetName(const std::string& rel);
bool parseVarAssignment(const std::string& text, std::string& name, std::string& value);
TemplateVar deriveVar(const std::string& name, const TemplateContext& ctx);
std::string renderTemplate(const std::string& text, const VarResolver& resolve, const std::string& sourceName, std::vector<VarUse>& uses);
std::vector<GeneratedHeader> planGeneratedHeaders(const ProjectInfo& proj, const TemplateContext& ctx, std::vector<Diagnostic>& diags);
}  // namespace wf
