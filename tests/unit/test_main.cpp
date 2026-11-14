// Minimal dependency-free unit tests. Run: scripts/test.sh
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "assets/assets.hpp"
#include "backend/backend.hpp"
#include "config/config.hpp"
#include "core/util.hpp"
#include "project/project.hpp"
#include "project/templates.hpp"
#include "runtime/server.hpp"
#include "runtime/wasm.hpp"

using namespace wf;

static int g_fail = 0, g_total = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_total;                                                                   \
    if (!(cond)) {                                                               \
      ++g_fail;                                                                  \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                            \
  } while (0)

static void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

static void testUtil() {
  CHECK(globMatch("*.c", "src/a.c"));
  CHECK(globMatch("src/*/x?.c", "src/mid/xy.c"));
  CHECK(!globMatch("*.c", "a.cpp"));
  CHECK(fnv1a(std::string("abc")) == fnv1a(std::string("abc")));
  CHECK(fnv1a(std::string("abc")) != fnv1a(std::string("abd")));
  CHECK(median({3, 1, 2}) == 2.0);
  CHECK(median({1, 2, 3, 4}) == 2.5);
  CHECK(humanBytes(1536) == "1.5 KB");
  CHECK(jsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
  const fs::path tmp = fs::temp_directory_path() / "wf-unit-util";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "in");
  CHECK(isInside(tmp, tmp / "in" / "x"));
  CHECK(!isInside(tmp / "in", tmp / "in" / ".." / "out"));   // path traversal
  CHECK(!isInside(tmp / "in", tmp / "other"));
  CHECK(writeFileAtomic(tmp / "f.txt", "hello"));
  CHECK(readFile(tmp / "f.txt").value_or("") == "hello");
  fs::remove_all(tmp);
}

static void testConfig() {
  std::string err;
  CHECK(validateKeyValue("workers", "4", err));
  CHECK(!validateKeyValue("workers", "0", err));
  CHECK(!validateKeyValue("workers", "abc; rm -rf /", err));
  CHECK(!validateKeyValue("profile", "turbo", err));
  CHECK(!validateKeyValue("assets", "transform", err));  // honest: not implemented
  CHECK(!validateKeyValue("name", "../evil", err));       // no path separators in names
  CHECK(!validateKeyValue("nonsense", "1", err));

  ProjectInfo cproj;  // C-only project
  HardwareInfo big;
  big.cores = 16; big.ramTotal = big.ramAvailable = 32ull << 30; big.environment = "linux";
  ResolvedConfig r = resolveConfig({}, big, cproj);
  CHECK(r.profile == Profile::Performance);
  CHECK(r.workers == 15);
  CHECK(r.strategy == Strategy::Execution);

  HardwareInfo phone;
  phone.cores = 8; phone.ramTotal = 6ull << 30; phone.ramAvailable = 2ull << 30; phone.environment = "termux";
  r = resolveConfig({}, phone, cproj);
  CHECK(r.profile == Profile::LowMemory);
  CHECK(r.workers <= 2);
  CHECK(r.strategy == Strategy::Compact);
  CHECK(r.memoryMb < 1000);  // never grabs most of the phone's RAM

  HardwareInfo unknown;  // nothing readable
  r = resolveConfig({}, unknown, cproj);
  CHECK(r.profile == Profile::LowMemory);
  CHECK(r.workers == 1);
  CHECK(r.memoryMb == 512);

  // HYBRID: user overrides one value, the rest stays automatic.
  ConfigMap cm = mergeLayers({}, {}, {{"workers", "2"}});
  r = resolveConfig(cm, big, cproj);
  CHECK(r.workers == 2);
  CHECK(r.profile == Profile::Performance);
  bool sawUser = false;
  for (const auto& d : r.decisions)
    if (d.key == "workers" && d.source == "cli") sawUser = true;
  CHECK(sawUser);

  // Memory budget caps workers: 1 GB budget / 700 MB per C++ job => 1 worker.
  ProjectInfo cxx;
  cxx.hasCxx = true;
  cm = mergeLayers({}, {}, {{"memory", "1024"}, {"profile", "performance"}});
  r = resolveConfig(cm, big, cxx);
  CHECK(r.workers == 1);

  // Layer precedence: cli > local > global
  cm = mergeLayers({{"workers", "1"}}, {{"workers", "2"}}, {{"workers", "3"}});
  CHECK(cm["workers"].value == "3" && cm["workers"].source == "cli");
  cm = mergeLayers({{"workers", "1"}}, {{"workers", "2"}}, {});
  CHECK(cm["workers"].value == "2" && cm["workers"].source == "local");

  const fs::path f = fs::temp_directory_path() / "wf-unit-cfg.ini";
  ConfigFile c = {{"profile", "low-memory"}, {"workers", "2"}};
  CHECK(saveConfigFile(f, c));
  ConfigFile back;
  std::vector<std::string> issues;
  CHECK(loadConfigFile(f, back, issues) && issues.empty() && back == c);
  write(f, "workers = 999\nbogus = 1\nprofile = auto\n");
  back.clear();
  loadConfigFile(f, back, issues);
  CHECK(issues.size() == 2 && back.size() == 1);  // bad lines are reported, good ones kept
  fs::remove(f);
}

static void testProject() {
  const fs::path root = fs::temp_directory_path() / "wf-unit-proj";
  fs::remove_all(root);
  write(root / "src/a.c", "#include \"a.h\"\n#include <SDL.h>\n#include <stdio.h>\nint a(void){return 1;}\n");
  write(root / "src/a.h", "#include \"deep/b.h\"\n");
  write(root / "src/deep/b.h", "#include \"a.h\"\n#include \"config.h\"\n");  // cycle + missing generated header
  write(root / "src/main.c", "#include <zlib.h>\nint main(void) { const char* n = \"doom1.wad\"; return n != 0; }\n");
  write(root / "src/proto.c", "int main(int argc, char** argv);\nint helper(void){return 0;}\n");
  write(root / "src/w.cpp", "#include <vector>\n#include <windows.h>\nvoid w(){}\n");
  write(root / "CMakeLists.txt", "project(x)\n");
  write(root / "dist/ignored.c", "int main(void){return 0;}\n");

  ProjectInfo P;
  std::string err;
  CHECK(discoverProject(root, {}, P, err));
  CHECK(P.sources.size() == 4);  // dist/ is skipped
  CHECK(P.hasC && P.hasCxx);
  CHECK(P.buildSystems.size() == 1 && P.buildSystems[0] == "cmake");
  int mains = 0;
  for (const auto& s : P.sources) mains += s.hasMain;
  CHECK(mains == 1);                       // one-line definition counts, a prototype does not
  CHECK(P.hasEntry());
  CHECK(P.depFiles.count("sdl2") && P.depFiles.count("zlib") && P.depFiles.count("windows-api"));
  CHECK(!P.depFiles.count("std"));
  CHECK(P.missing.count("config.h"));      // generated header detected as missing
  CHECK(P.referencedNames.count("doom1.wad"));
  auto t = transitiveHeaders(P, "src/a.c");
  CHECK(t.count("src/a.h") && t.count("src/deep/b.h"));  // survives the include cycle
  fs::remove_all(root);

  std::string kind;
  CHECK(classifyExternalHeader("stdio.h", kind).empty() && kind == "std");
  CHECK(classifyExternalHeader("vector", kind).empty() && kind == "std");
  CHECK(classifyExternalHeader("sys/stat.h", kind).empty() && kind == "posix");
  CHECK(classifyExternalHeader("SDL2/SDL_mixer.h", kind) == "sdl2_mixer");
  CHECK(classifyExternalHeader("mystery.h", kind).empty() && kind == "unknown");
}

static void testAssets() {
  const fs::path root = fs::temp_directory_path() / "wf-unit-assets";
  fs::remove_all(root);
  write(root / "a.wad", std::string("IWAD") + std::string(12, '\0'));
  write(root / "fake.png", "this is not a png at all");
  write(root / "real.png", std::string("\x89PNG\r\n\x1a\n", 8) + "xxxx");
  write(root / "snd.wav", std::string("RIFF\0\0\0\0WAVEfmt ", 16));
  write(root / "lib.so", std::string("\x7f" "ELF", 4) + std::string(60, '\0'));
  write(root / "README.md", "# hi");
  // ar archive whose first member is a wasm object
  std::string ar = "!<arch>\n";
  std::string hdr(60, ' ');
  hdr.replace(0, 5, "a.o/ ");
  hdr.replace(48, 2, "8 ");
  hdr.replace(58, 2, "`\n");
  ar += hdr + std::string("\0asm\1\0\0\0", 8);
  write(root / "libwasm.a", ar);
  std::string ar2 = "!<arch>\n" + hdr + std::string("\x7f" "ELF\2\1\1\0", 8);
  write(root / "libnative.a", ar2);

  CHECK(classifyAsset(root, "a.wad").kind == AssetKind::Wad);
  CHECK(classifyAsset(root, "a.wad").signature == "IWAD");
  CHECK(classifyAsset(root, "real.png").kind == AssetKind::Image);
  CHECK(!classifyAsset(root, "fake.png").note.empty());  // extension without matching signature is flagged
  CHECK(classifyAsset(root, "snd.wav").kind == AssetKind::Audio);
  CHECK(classifyAsset(root, "lib.so").kind == AssetKind::SharedLib);
  CHECK(classifyAsset(root, "README.md").kind == AssetKind::Document);
  CHECK(classifyAsset(root, "libwasm.a").archiveContent == "wasm");
  CHECK(classifyAsset(root, "libnative.a").archiveContent == "native");

  std::vector<AssetInfo> v;
  for (const char* n : {"a.wad", "real.png", "lib.so", "README.md", "libwasm.a", "libnative.a"}) v.push_back(classifyAsset(root, n));
  decideAssets(v, AssetMode::Auto, {"a.wad", "real.png"}, {});
  CHECK(v[0].action == AssetAction::External);  // WAD: kept as-is, mounted at runtime
  CHECK(v[1].action == AssetAction::Embed);     // tiny referenced image
  CHECK(v[2].action == AssetAction::Skip);      // native library cannot run in wasm
  CHECK(v[3].action == AssetAction::Skip);      // docs are not runtime data
  CHECK(v[4].action == AssetAction::Link);
  CHECK(v[5].action == AssetAction::Skip);
  decideAssets(v, AssetMode::Auto, {}, {});     // nothing references them any more
  CHECK(v[0].action == AssetAction::Skip);
  decideAssets(v, AssetMode::Auto, {}, {"*.wad"});
  CHECK(v[0].action == AssetAction::External);  // --asset forces inclusion
  fs::remove_all(root);
}

static void testBackend() {
  auto em = makeBackend("emscripten");
  auto wasi = makeBackend("wasi-sdk");
  CHECK(em && wasi && !makeBackend("nope"));
  CHECK(em->supports(OutputKind::Web) && !wasi->supports(OutputKind::Web));  // Web vs WASI are different targets
  CHECK(em->dependency("sdl2").flags == std::vector<std::string>{"-sUSE_SDL=2"});
  CHECK(em->dependency("sdl2_mixer").supported);
  CHECK(!em->dependency("windows-api").supported);
  CHECK(!wasi->dependency("sdl2").supported);
  CHECK(em->optimization(Strategy::Compact).compile[0] == "-Oz");
  CHECK(em->optimization(Strategy::Execution).compile[0] == "-O3");
  CHECK(em->primaryOutput("g", OutputKind::Web, false) == "g.js");
  CHECK(em->primaryOutput("g", OutputKind::Web, true) == "g.html");
  CHECK(em->artifacts("g", OutputKind::WasmOnly, false) == std::vector<std::string>{"g.wasm"});
}


// ---- tiny wasm assembler for tests --------------------------------------------------------------
static std::string leb(std::uint32_t n) {
  std::string o;
  do {
    unsigned char b = n & 0x7F;
    n >>= 7;
    o.push_back(static_cast<char>(n ? (b | 0x80) : b));
  } while (n);
  return o;
}
static std::string nm(const std::string& s) { return leb(static_cast<std::uint32_t>(s.size())) + s; }
static std::string sec(int id, const std::string& payload) { return std::string(1, static_cast<char>(id)) + leb(static_cast<std::uint32_t>(payload.size())) + payload; }
static std::string goodModule(bool wasi, bool shared = false) {
  const std::string hdr("\0asm\1\0\0\0", 8);
  std::string m = hdr;
  m += sec(1, leb(1) + std::string("\x60\x00\x00", 3));
  m += sec(2, leb(1) + nm(wasi ? "wasi_snapshot_preview1" : "env") + nm(wasi ? "fd_write" : "emscripten_resize_heap") + std::string("\x00", 1) + leb(0));
  m += sec(3, leb(1) + leb(0));
  m += sec(5, leb(1) + std::string(shared ? "\x03" : "\x01", 1) + leb(256) + leb(512));
  m += sec(7, leb(1) + nm("main") + std::string("\x00", 1) + leb(1));
  m += sec(10, leb(1) + leb(2) + std::string("\x00\x0b", 2));
  m += sec(11, leb(1) + std::string("\x00\x41\x00\x0b", 4) + leb(5) + "hello");
  return m;
}

static void testWasm() {
  WasmInfo w = parseWasm(goodModule(false));
  CHECK(w.valid);
  CHECK(w.imports.size() == 1 && w.imports[0].module == "env");
  CHECK(w.exports.size() == 1 && w.exports[0].name == "main");
  CHECK(w.functionsDefined == 1 && w.functionsImported == 1);
  CHECK(w.memoryMinPages == 256 && w.memoryMaxPages == 512 && w.memoryHasMax);
  CHECK(w.dataBytes == 5 && w.dataSegments == 1);
  CHECK(classifyWasmTarget(w) == "emscripten");
  CHECK(wasmDiagnostics(w, true).empty());

  WasmInfo wasi = parseWasm(goodModule(true));
  CHECK(wasi.valid && wasi.importsWasi() && classifyWasmTarget(wasi) == "wasi");
  bool webErr = false;
  for (const auto& d : wasmDiagnostics(wasi, true)) webErr |= d.code == "E-WASM-WASI-IN-WEB";
  CHECK(webErr);                                  // WASI module must not be shipped as a browser build
  CHECK(wasmDiagnostics(wasi, false).empty());    // ...but is fine as a plain WASI module

  WasmInfo sh = parseWasm(goodModule(false, true));
  CHECK(sh.valid && sh.memoryShared);
  bool sawShared = false;
  for (const auto& d : wasmDiagnostics(sh, true)) sawShared |= d.code == "W-WASM-SHARED-MEMORY";
  CHECK(sawShared);

  // Malformed input must produce an error, never a crash or a hang.
  CHECK(!parseWasm("").valid);
  CHECK(!parseWasm("not wasm at all").valid);
  CHECK(!parseWasm(std::string("\0asm\2\0\0\0", 8)).valid);                          // unsupported version
  CHECK(!parseWasm(std::string("\0asm\1\0\0\0\x01\xff", 10)).valid);                // truncated section header
  CHECK(!parseWasm(std::string("\0asm\1\0\0\0", 8) + std::string("\x01\x7f", 2)).valid);  // size exceeds file
  CHECK(!parseWasm(std::string("\0asm\1\0\0\0", 8) + sec(2, std::string("\xff\xff\xff\xff\xff\xff", 6))).valid);  // over-long LEB
  CHECK(parseWasm(std::string("\0asm\1\0\0\0", 8)).valid);                           // empty module is valid

  // Deterministic mutation fuzzing (run under ASan/UBSan in CI).
  const std::string base = goodModule(false);
  unsigned long long seed = 88172645463325252ULL;
  auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
  int parsedOk = 0;
  for (int i = 0; i < 4000; ++i) {
    std::string m = base;
    const int edits = 1 + static_cast<int>(rnd() % 4);
    for (int e = 0; e < edits; ++e) m[rnd() % m.size()] = static_cast<char>(rnd() & 0xFF);
    if (rnd() % 4 == 0) m.resize(rnd() % m.size());
    parsedOk += parseWasm(m).valid ? 1 : 0;
  }
  CHECK(parsedOk >= 0);  // reaching this line without a crash is the assertion
}

static void testServer() {
  std::string rel;
  CHECK(decodeRequestPath("/", rel) && rel.empty());
  CHECK(decodeRequestPath("/game/app.js?v=1#x", rel) && rel == "game/app.js");
  CHECK(decodeRequestPath("//a//b/./c", rel) && rel == "a/b/c");
  CHECK(decodeRequestPath("/my%20file.wad", rel) && rel == "my file.wad");
  CHECK(!decodeRequestPath("/../etc/passwd", rel));
  CHECK(!decodeRequestPath("/a/../../b", rel));
  CHECK(!decodeRequestPath("/%2e%2e/x", rel));        // encoded dot-dot
  CHECK(!decodeRequestPath("/%2E%2E%2fx", rel));      // encoded slash too
  CHECK(!decodeRequestPath("/a%00b", rel));           // NUL injection
  CHECK(!decodeRequestPath("/a%5Cb", rel));           // backslash (Windows separator)
  CHECK(!decodeRequestPath("/a\\b", rel));
  CHECK(!decodeRequestPath("/%zz", rel));             // invalid escape
  CHECK(!decodeRequestPath("/abc%4", rel));           // truncated escape
  CHECK(!decodeRequestPath("/C:/Windows", rel));      // drive letter
  CHECK(!decodeRequestPath("relative/path", rel));
  CHECK(!decodeRequestPath("", rel));
  CHECK(mimeTypeFor("game.wasm") == "application/wasm");  // required by WebAssembly.instantiateStreaming
  CHECK(mimeTypeFor("DOOM.WAD") == "application/octet-stream");
  CHECK(mimeTypeFor("a.js").find("javascript") != std::string::npos);
}

static void testTemplates() {
  CHECK(isTruthy("1") && isTruthy("ON") && isTruthy("yes") && isTruthy("\"x\""));
  CHECK(!isTruthy("") && !isTruthy("0") && !isTruthy("OFF") && !isTruthy("FALSE") && !isTruthy("FOO-NOTFOUND") && !isTruthy("NOTFOUND"));
  CHECK(templateTargetName("src/config.h.cin") == "config.h");
  CHECK(templateTargetName("config.h.in") == "config.h");
  CHECK(templateTargetName("x/foo.hpp.cmake") == "foo.hpp");
  CHECK(templateTargetName("Makefile.in").empty() && templateTargetName("notes.txt").empty() && templateTargetName("config.h").empty());
  std::string n, v;
  CHECK(parseVarAssignment("A=1", n, v) && n == "A" && v == "1");
  CHECK(parseVarAssignment("FLAG", n, v) && v == "1");
  CHECK(parseVarAssignment("S=a b=c", n, v) && v == "a b=c");
  CHECK(!parseVarAssignment("1bad=2", n, v) && !parseVarAssignment("a-b=2", n, v) && !parseVarAssignment("=2", n, v));

  TemplateVars vars;
  vars["ON"] = {true, "1", "user", ""};
  vars["OFF"] = {true, "0", "user", ""};
  vars["NAME"] = {true, "My Game", "user", ""};
  vars["NUM"] = {true, "42", "user", ""};
  VarResolver res = [&](const std::string& k) { auto it = vars.find(k); return it == vars.end() ? TemplateVar{} : it->second; };
  std::vector<VarUse> uses;
  const std::string out = renderTemplate(
      "#cmakedefine ON\n#cmakedefine OFF\n#cmakedefine MISSING\n#cmakedefine NUM @NUM@\n#cmakedefine01 ON\n#cmakedefine01 OFF\n"
      "#define TITLE \"@NAME@\"\n#define EMPTY \"@NOPE@\"\n#undef NAME\n#undef NUM\n#undef OFF\n  int x = a@b; /* not a var */\n", res, "t.h.in", uses);
  auto has = [&](const std::string& line) { return out.find("\n" + line + "\n") != std::string::npos; };
  CHECK(has("#define ON"));
  CHECK(has("/* #undef OFF */"));
  CHECK(has("/* #undef MISSING */"));
  CHECK(has("#define NUM 42"));
  CHECK(has("#define TITLE \"My Game\""));
  CHECK(has("#define EMPTY \"\""));              // unknown @VAR@ becomes empty, as in CMake
  CHECK(has("#define NAME \"My Game\""));         // autotools #undef of a string: quoted automatically
  CHECK(has("#define NUM 42"));
  CHECK(has("  int x = a@b; /* not a var */"));    // stray '@' is left alone
  CHECK(out.find("#cmakedefine") == std::string::npos);
  CHECK(out.find("Generated by WasmForge from t.h.in") != std::string::npos);
  CHECK(renderTemplate("a\r\n#cmakedefine ON\r\n", res, "t", uses).find("#define ON\r\n") != std::string::npos);  // CRLF preserved

  TemplateContext ctx;
  ctx.projectName = "Demo";
  ctx.depSupported = {{"libpng", true}, {"libsamplerate", false}};
  CHECK(deriveVar("HAVE_LIBPNG", ctx).defined && deriveVar("HAVE_LIBPNG", ctx).origin == "derived");
  CHECK(!deriveVar("HAVE_LIBSAMPLERATE", ctx).defined);
  CHECK(deriveVar("HAVE_UNISTD_H", ctx).origin == "guess");
  CHECK(!deriveVar("HAVE_SOMETHING_ODD_H", ctx).defined && deriveVar("HAVE_SOMETHING_ODD_H", ctx).origin == "unset");
  CHECK(!deriveVar("WORDS_BIGENDIAN", ctx).defined);
  CHECK(deriveVar("PACKAGE_NAME", ctx).value == "Demo" && deriveVar("PACKAGE_NAME", ctx).origin == "placeholder");

  // Unsafe include strings must never decide where a file is written.
  ProjectInfo P;
  P.root = fs::temp_directory_path();
  P.otherFiles = {"config.h.in"};
  P.missing["../../evil/config.h"] = {"a.c"};
  P.missing["/abs/config.h"] = {"a.c"};
  ctx.activeFiles = {"a.c"};
  std::vector<Diagnostic> d;
  CHECK(planGeneratedHeaders(P, ctx, d).empty());
}

static void testWasmDiff() {
  WasmInfo a = parseWasm(goodModule(false)), b = parseWasm(goodModule(true));
  const std::string d = diffWasm(a, b);
  CHECK(d.find("wasi_snapshot_preview1.fd_write") != std::string::npos);
  CHECK(d.find("target        emscripten -> wasi") != std::string::npos);
  CHECK(diffWasm(a, a).find("identical") != std::string::npos);
  CHECK(diffWasm(a, parseWasm("junk")).find("cannot compare") != std::string::npos);
}

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> suites = {
      {"util", testUtil}, {"config", testConfig}, {"project", testProject}, {"assets", testAssets}, {"backend", testBackend}, {"wasm", testWasm}, {"server", testServer}, {"templates", testTemplates}, {"wasm-diff", testWasmDiff}};
  for (const auto& [name, fn] : suites) {
    const int before = g_fail;
    fn();
    std::printf("%-8s %s\n", name, g_fail == before ? "ok" : "FAILED");
  }
  std::printf("\n%d/%d unit checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}
