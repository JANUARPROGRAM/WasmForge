#pragma once
#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "core/util.hpp"

namespace wf {

struct BackendProbe {
  bool available = false;
  std::string version;
  fs::path cc, cxx;
  std::string note;  // why it is unavailable
};

struct OptFlags {
  std::vector<std::string> compile, link;
  std::string summary;
};

struct DepSupport {
  bool supported = false;
  std::vector<std::string> flags;
  std::string note;
};

struct CompileJob {
  std::string source, object;  // paths as strings; the process runs with cwd = project root
  bool cxx = false;
  std::vector<std::string> includeDirs, defines, flags;
};

struct LinkJob {
  std::vector<std::string> inputs;
  std::string output;
  OutputKind kind = OutputKind::Web;
  bool cxx = false, noEntry = false, needsFilesystem = false, html = false;
  std::vector<std::string> flags, preJs, embedFiles;  // embedFiles: "src@/virtual/path"
};

// A backend turns WasmForge's internal, backend-neutral description into concrete tool invocations.
// The core never builds a command line itself and never names emcc/clang directly.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual std::string id() const = 0;
  virtual std::string displayName() const = 0;
  virtual bool supports(OutputKind) const = 0;
  virtual const BackendProbe& probe() const = 0;
  virtual OptFlags optimization(Strategy) const = 0;
  virtual DepSupport dependency(const std::string& depId) const = 0;
  virtual std::vector<std::string> compileArgv(const CompileJob&) const = 0;
  virtual std::vector<std::string> linkArgv(const LinkJob&) const = 0;
  virtual std::string primaryOutput(const std::string& name, OutputKind, bool html) const = 0;
  virtual std::vector<std::string> artifacts(const std::string& name, OutputKind, bool html) const = 0;
};

std::vector<std::string> backendIds();
std::unique_ptr<Backend> makeBackend(const std::string& id);
}  // namespace wf
