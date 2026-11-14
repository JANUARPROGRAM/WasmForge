#include "backend/backend.hpp"

#include <optional>

#include "platform/platform.hpp"

namespace wf {
namespace {

std::string toolName(const fs::path& p, const char* fallback) { return p.empty() ? std::string(fallback) : pathStr(p); }

std::string firstLine(const std::string& s) {
  const auto n = s.find('\n');
  return trim(n == std::string::npos ? s : s.substr(0, n));
}

// ------------------------------------------------------------------------------------------------
// Emscripten: the only backend that can target browsers today.
// ------------------------------------------------------------------------------------------------
class EmscriptenBackend : public Backend {
 public:
  std::string id() const override { return "emscripten"; }
  std::string displayName() const override { return "Emscripten (emcc / em++)"; }
  bool supports(OutputKind k) const override { return k == OutputKind::Web || k == OutputKind::WasmOnly || k == OutputKind::Runtime; }

  const BackendProbe& probe() const override {
    if (probe_) return *probe_;
    BackendProbe p;
    const auto cc = findInPath("emcc");
    const auto cxx = findInPath("em++");
    if (!cc || !cxx) {
      p.note = "emcc / em++ were not found in PATH (activate the Emscripten SDK, e.g. `source emsdk_env.sh`, or install Emscripten)";
    } else {
      p.cc = *cc;
      p.cxx = *cxx;
      const ProcessResult r = runProcess({pathStr(*cc), "--version"}, {}, true);
      if (!r.started) p.note = r.error;
      else if (r.exitCode != 0) p.note = "emcc --version exited with code " + std::to_string(r.exitCode);
      else {
        p.available = true;
        p.version = firstLine(r.output);
      }
    }
    probe_ = p;
    return *probe_;
  }

  OptFlags optimization(Strategy s) const override {
    OptFlags o;
    switch (s) {
      case Strategy::None: o.compile = {"-O0"}; o.link = {"-O0"}; o.summary = "-O0: no optimisation (fastest build, easiest debugging)"; break;
      case Strategy::Compact: o.compile = {"-Oz", "-ffunction-sections", "-fdata-sections"}; o.link = {"-Oz"}; o.summary = "-Oz + function/data sections: smallest output, link-time dead-code removal"; break;
      case Strategy::Execution: o.compile = {"-O3"}; o.link = {"-O3"}; o.summary = "-O3: fastest generated code"; break;
      case Strategy::Memory: o.compile = {"-Os", "-ffunction-sections", "-fdata-sections"}; o.link = {"-Os"}; o.summary = "-Os: small code, lower compiler memory pressure"; break;
      default: o.compile = {"-O2"}; o.link = {"-O2"}; o.summary = "-O2: speed/size balance"; break;
    }
    return o;
  }

  DepSupport dependency(const std::string& d) const override {
    struct Port { const char* id; const char* flag; };
    static const Port ports[] = {{"sdl2", "-sUSE_SDL=2"},         {"sdl2_mixer", "-sUSE_SDL_MIXER=2"}, {"sdl2_net", "-sUSE_SDL_NET=2"},
                                 {"sdl2_image", "-sUSE_SDL_IMAGE=2"}, {"sdl2_ttf", "-sUSE_SDL_TTF=2"},   {"zlib", "-sUSE_ZLIB=1"},
                                 {"libpng", "-sUSE_LIBPNG=1"},    {"libjpeg", "-sUSE_LIBJPEG=1"},      {"freetype", "-sUSE_FREETYPE=1"},
                                 {"ogg", "-sUSE_OGG=1"},          {"vorbis", "-sUSE_VORBIS=1"}};
    DepSupport s;
    for (const auto& p : ports)
      if (d == p.id) {
        s.supported = true;
        s.flags = {p.flag};
        s.note = "Emscripten port (downloaded and cached by Emscripten on first use; needs network once)";
        return s;
      }
    if (d == "emscripten") { s.supported = true; s.note = "provided by the backend"; return s; }
    if (d == "threads") { s.supported = true; s.note = "pthread.h found; threads are NOT enabled automatically (browser pthreads need cross-origin isolation). Add --flag -pthread if the program really uses threads"; return s; }
    if (d == "opengl") { s.note = "OpenGL needs manual Emscripten GL flags (e.g. -sFULL_ES2=1); not configured automatically"; return s; }
    if (d == "windows-api") { s.note = "Windows API headers cannot be built for WebAssembly (often guarded by #ifdef _WIN32)"; return s; }
    if (d == "x11") { s.note = "X11 is not available on WebAssembly"; return s; }
    s.note = "no Emscripten port is known for this library; it must be built from source for wasm";
    return s;
  }

  std::vector<std::string> compileArgv(const CompileJob& j) const override {
    std::vector<std::string> a = {j.cxx ? toolName(probe().cxx, "em++") : toolName(probe().cc, "emcc"), "-c", j.source};
    for (const auto& d : j.defines) a.push_back("-D" + d);
    for (const auto& i : j.includeDirs) a.push_back("-I" + i);
    for (const auto& f : j.flags) a.push_back(f);
    a.push_back("-o");
    a.push_back(j.object);
    return a;
  }

  std::vector<std::string> linkArgv(const LinkJob& j) const override {
    std::vector<std::string> a = {j.cxx ? toolName(probe().cxx, "em++") : toolName(probe().cc, "emcc")};
    for (const auto& i : j.inputs) a.push_back(i);
    for (const auto& f : j.flags) a.push_back(f);
    if (j.kind == OutputKind::WasmOnly) a.push_back("-sSTANDALONE_WASM=1");
    else a.push_back("-sALLOW_MEMORY_GROWTH=1");
    if (j.noEntry) a.push_back("--no-entry");
    if (j.needsFilesystem) {
      a.push_back("-sFORCE_FILESYSTEM=1");
      a.push_back("-sEXPORTED_RUNTIME_METHODS=FS,addRunDependency,removeRunDependency");
    }
    for (const auto& p : j.preJs) { a.push_back("--pre-js"); a.push_back(p); }
    for (const auto& e : j.embedFiles) { a.push_back("--embed-file"); a.push_back(e); }
    a.push_back("-o");
    a.push_back(j.output);
    return a;
  }

  std::string primaryOutput(const std::string& n, OutputKind k, bool html) const override {
    if (k == OutputKind::WasmOnly) return n + ".wasm";
    return html && k == OutputKind::Web ? n + ".html" : n + ".js";
  }

  std::vector<std::string> artifacts(const std::string& n, OutputKind k, bool html) const override {
    if (k == OutputKind::WasmOnly) return {n + ".wasm"};
    std::vector<std::string> v;
    if (html && k == OutputKind::Web) v.push_back(n + ".html");
    v.push_back(n + ".js");
    v.push_back(n + ".wasm");
    return v;
  }

 private:
  mutable std::optional<BackendProbe> probe_;
};

// ------------------------------------------------------------------------------------------------
// WASI SDK: clang --target=wasm32-wasi. Produces .wasm for WASI runtimes (wasmtime, wasmer, ...),
// NOT for browsers. Included to prove the abstraction is real and to cover the Web/WASI distinction.
// ------------------------------------------------------------------------------------------------
class WasiBackend : public Backend {
 public:
  std::string id() const override { return "wasi-sdk"; }
  std::string displayName() const override { return "WASI SDK (clang --target=wasm32-wasi)"; }
  bool supports(OutputKind k) const override { return k == OutputKind::WasmOnly; }

  const BackendProbe& probe() const override {
    if (probe_) return *probe_;
    BackendProbe p;
    const std::string sdk = envVar("WASI_SDK_PATH");
    if (sdk.empty()) {
      p.note = "set WASI_SDK_PATH to the wasi-sdk installation directory";
    } else {
      const fs::path root = fromUtf8(sdk);
      const std::string exe = osName() == "windows" ? ".exe" : "";
      const fs::path cc = root / "bin" / ("clang" + exe), cxx = root / "bin" / ("clang++" + exe);
      std::error_code ec;
      sysroot_ = root / "share" / "wasi-sysroot";
      if (!fs::exists(cc, ec) || !fs::exists(cxx, ec)) p.note = "clang / clang++ not found under " + sdk + "/bin";
      else if (!fs::exists(sysroot_, ec)) p.note = "wasi-sysroot not found under " + sdk + "/share";
      else {
        p.cc = cc;
        p.cxx = cxx;
        const ProcessResult r = runProcess({pathStr(cc), "--version"}, {}, true);
        if (!r.started || r.exitCode != 0) p.note = r.started ? "clang --version failed" : r.error;
        else {
          p.available = true;
          p.version = firstLine(r.output);
        }
      }
    }
    probe_ = p;
    return *probe_;
  }

  OptFlags optimization(Strategy s) const override {
    OptFlags o;
    switch (s) {
      case Strategy::None: o.compile = o.link = {"-O0"}; o.summary = "-O0"; break;
      case Strategy::Compact: o.compile = {"-Oz", "-ffunction-sections", "-fdata-sections"}; o.link = {"-Oz", "-Wl,--gc-sections"}; o.summary = "-Oz + wasm-ld --gc-sections"; break;
      case Strategy::Execution: o.compile = o.link = {"-O3"}; o.summary = "-O3"; break;
      case Strategy::Memory: o.compile = {"-Os", "-ffunction-sections", "-fdata-sections"}; o.link = {"-Os", "-Wl,--gc-sections"}; o.summary = "-Os + wasm-ld --gc-sections"; break;
      default: o.compile = o.link = {"-O2"}; o.summary = "-O2"; break;
    }
    return o;
  }

  DepSupport dependency(const std::string& d) const override {
    DepSupport s;
    if (d == "threads") { s.supported = true; s.note = "pthread.h found; WASI threads are not enabled automatically"; return s; }
    s.note = "no ready-made WASI port of this library: it must be cross-compiled and passed with --flag";
    return s;
  }

  std::vector<std::string> compileArgv(const CompileJob& j) const override {
    probe();
    std::vector<std::string> a = {j.cxx ? toolName(probe().cxx, "clang++") : toolName(probe().cc, "clang"), "--target=wasm32-wasi", "--sysroot=" + pathStr(sysroot_), "-c", j.source};
    for (const auto& d : j.defines) a.push_back("-D" + d);
    for (const auto& i : j.includeDirs) a.push_back("-I" + i);
    for (const auto& f : j.flags) a.push_back(f);
    a.push_back("-o");
    a.push_back(j.object);
    return a;
  }

  std::vector<std::string> linkArgv(const LinkJob& j) const override {
    probe();
    std::vector<std::string> a = {j.cxx ? toolName(probe().cxx, "clang++") : toolName(probe().cc, "clang"), "--target=wasm32-wasi", "--sysroot=" + pathStr(sysroot_)};
    for (const auto& i : j.inputs) a.push_back(i);
    for (const auto& f : j.flags) a.push_back(f);
    if (j.noEntry) { a.push_back("-mexec-model=reactor"); a.push_back("-Wl,--no-entry"); }
    a.push_back("-o");
    a.push_back(j.output);
    return a;
  }

  std::string primaryOutput(const std::string& n, OutputKind, bool) const override { return n + ".wasm"; }
  std::vector<std::string> artifacts(const std::string& n, OutputKind, bool) const override { return {n + ".wasm"}; }

 private:
  mutable std::optional<BackendProbe> probe_;
  mutable fs::path sysroot_;
};

}  // namespace

std::vector<std::string> backendIds() { return {"emscripten", "wasi-sdk"}; }

std::unique_ptr<Backend> makeBackend(const std::string& id) {
  if (id == "emscripten") return std::make_unique<EmscriptenBackend>();
  if (id == "wasi-sdk") return std::make_unique<WasiBackend>();
  return nullptr;
}
}  // namespace wf
