#pragma once
#include <ostream>
#include <string>
#include <vector>

namespace wf {
enum class Level { Info, Warning, Error };

struct Diagnostic {
  Level level = Level::Info;
  std::string code;     // stable identifier, e.g. E-NOBACKEND
  std::string message;  // what happened
  std::string hint;     // what the user can do about it
};

inline const char* levelName(Level l) {
  return l == Level::Error ? "error" : l == Level::Warning ? "warning" : "info";
}

inline void printDiagnostics(std::ostream& os, const std::vector<Diagnostic>& ds) {
  for (const auto& d : ds) {
    os << levelName(d.level) << "[" << d.code << "]: " << d.message << "\n";
    if (!d.hint.empty()) os << "    hint: " << d.hint << "\n";
  }
}
}  // namespace wf
