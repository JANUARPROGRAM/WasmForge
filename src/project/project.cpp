#include "project/project.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace wf {
namespace {

bool skipDirName(const std::string& n) {
  static const std::set<std::string> skip = {".git", ".hg", ".svn", ".wasmforge", "node_modules", "dist",
                                             "CMakeFiles", "__pycache__", ".idea", ".vscode"};
  return skip.count(n) > 0;
}

bool junkFile(const std::string& n, const std::string& ext) {
  static const std::set<std::string> junk = {".o", ".obj", ".d", ".tmp", ".swp", ".orig", ".rej", ".pyc", ".log"};
  return junk.count(ext) > 0 || startsWith(n, ".#") || n == ".DS_Store";
}

std::string sanitizeName(std::string s) {
  std::string o;
  for (char c : s) o.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_');
  if (o.empty() || o == "_" || o == "__") o = "app";
  if (o.size() > 64) o.resize(64);
  return o;
}

std::string_view ltrimView(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  return s;
}

struct Scan {
  std::vector<std::pair<std::string, bool>> includes;  // (name, quoted?)
  bool hasMain = false;
  std::vector<std::string> literals;                   // file-name-like string literals (lower-case basenames)
};

void collectLiterals(std::string_view line, std::vector<std::string>& out) {
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] != '"') continue;
    std::string lit;
    std::size_t j = i + 1;
    for (; j < line.size() && line[j] != '"'; ++j) {
      if (line[j] == '\\') ++j;
      else lit.push_back(line[j]);
    }
    i = j;
    if (lit.size() < 3 || lit.size() > 96) continue;
    const auto dot = lit.rfind('.');
    if (dot == std::string::npos || lit.size() - dot - 1 < 1 || lit.size() - dot - 1 > 5) continue;
    bool ok = true;
    for (char c : lit)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-' || c == '/')) ok = false;
    if (!ok) continue;
    const auto slash = lit.rfind('/');
    out.push_back(lower(slash == std::string::npos ? lit : lit.substr(slash + 1)));
  }
}

// Deliberately simple scanner: ignores #if/#ifdef (so dependencies are an over-approximation,
// which is the safe direction for incremental builds) and only understands whole-line block comments.
Scan scanText(const std::string& t) {
  Scan s;
  bool inBlock = false;
  std::size_t pos = 0;
  while (pos < t.size()) {
    std::size_t e = t.find('\n', pos);
    if (e == std::string::npos) e = t.size();
    std::string_view l(t.data() + pos, e - pos);
    pos = e + 1;
    l = ltrimView(l);
    if (inBlock) {
      const auto c = l.find("*/");
      if (c == std::string_view::npos) continue;
      inBlock = false;
      l = ltrimView(l.substr(c + 2));
    }
    if (l.empty()) continue;
    if (l.substr(0, 2) == "/*") {
      if (l.find("*/") == std::string_view::npos) inBlock = true;
      continue;
    }
    if (l.substr(0, 2) == "//") continue;
    if (l[0] == '#') {
      l.remove_prefix(1);
      l = ltrimView(l);
      if (l.substr(0, 7) == "include" && (l.size() == 7 || !std::isalnum(static_cast<unsigned char>(l[7])))) {
        l = ltrimView(l.substr(7));
        if (!l.empty() && (l[0] == '"' || l[0] == '<')) {
          const char close = l[0] == '"' ? '"' : '>';
          const auto end = l.find(close, 1);
          if (end != std::string_view::npos) s.includes.emplace_back(std::string(l.substr(1, end - 1)), l[0] == '"');
        }
      }
      continue;
    }
    if (!s.hasMain && (l.substr(0, 4) == "int " || l.substr(0, 5) == "void ")) {
      const auto m = l.find("main");
      if (m != std::string_view::npos && (m == 0 || l[m - 1] == ' ' || l[m - 1] == '\t' || l[m - 1] == '*')) {
        auto rest = ltrimView(l.substr(m + 4));
        std::string_view tl = l;  // a prototype ends with ';' - a one-line definition ends with '}' or '{'
        while (!tl.empty() && (tl.back() == ' ' || tl.back() == '\t' || tl.back() == '\r')) tl.remove_suffix(1);
        if (!rest.empty() && rest[0] == '(' && !(tl.size() && tl.back() == ';')) s.hasMain = true;
      }
    }
    collectLiterals(l, s.literals);
  }
  return s;
}

std::string normalizeRel(const std::string& dir, const std::string& inc) {
  fs::path p = (dir.empty() || dir == ".") ? fs::path(inc) : fs::path(dir) / inc;
  std::string n = p.lexically_normal().generic_string();
  return n;
}

std::string parentDir(const std::string& rel) {
  const auto s = rel.rfind('/');
  return s == std::string::npos ? std::string() : rel.substr(0, s);
}

}  // namespace

bool shouldSkipDirName(const std::string& name) { return skipDirName(name); }

std::string classifyExternalHeader(const std::string& inc, std::string& kind) {
  static const std::set<std::string> cstd = {
      "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h", "limits.h",
      "locale.h", "math.h", "setjmp.h", "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h", "stdbool.h",
      "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h", "string.h", "tgmath.h", "time.h",
      "uchar.h", "wchar.h", "wctype.h", "malloc.h", "alloca.h", "endian.h"};
  static const std::set<std::string> posix = {
      "unistd.h", "fcntl.h", "dirent.h", "netdb.h", "poll.h", "sched.h", "semaphore.h", "strings.h", "utime.h",
      "dlfcn.h", "libgen.h", "glob.h", "getopt.h", "termios.h", "regex.h", "fnmatch.h", "ftw.h", "grp.h",
      "pwd.h", "syslog.h", "wordexp.h", "execinfo.h", "ifaddrs.h", "langinfo.h", "iconv.h"};
  struct Dep { const char* prefix; const char* id; };
  static const Dep deps[] = {
      {"SDL2/SDL_mixer.h", "sdl2_mixer"}, {"SDL_mixer.h", "sdl2_mixer"}, {"SDL2/SDL_net.h", "sdl2_net"},
      {"SDL_net.h", "sdl2_net"},          {"SDL2/SDL_image.h", "sdl2_image"}, {"SDL_image.h", "sdl2_image"},
      {"SDL2/SDL_ttf.h", "sdl2_ttf"},     {"SDL_ttf.h", "sdl2_ttf"},         {"SDL2/", "sdl2"},
      {"SDL.h", "sdl2"},                  {"SDL_", "sdl2"},                  {"zlib.h", "zlib"},
      {"png.h", "libpng"},                {"jpeglib.h", "libjpeg"},          {"ft2build.h", "freetype"},
      {"freetype/", "freetype"},          {"ogg/", "ogg"},                   {"vorbis/", "vorbis"},
      {"samplerate.h", "libsamplerate"},  {"fluidsynth.h", "fluidsynth"},    {"curl/", "curl"},
      {"openssl/", "openssl"},            {"GL/", "opengl"},                 {"GLES2/", "opengl"},
      {"GLES3/", "opengl"},               {"EGL/", "opengl"},                {"X11/", "x11"},
      {"windows.h", "windows-api"},       {"winsock2.h", "windows-api"},     {"ws2tcpip.h", "windows-api"},
      {"direct.h", "windows-api"},        {"io.h", "windows-api"},           {"pthread.h", "threads"},
      {"emscripten.h", "emscripten"},     {"emscripten/", "emscripten"}};
  if (cstd.count(inc)) { kind = "std"; return {}; }
  if (posix.count(inc) || startsWith(inc, "sys/") || startsWith(inc, "netinet/") || startsWith(inc, "arpa/") ||
      startsWith(inc, "linux/")) { kind = "posix"; return {}; }
  for (const auto& d : deps)
    if (startsWith(inc, d.prefix)) { kind = "dep"; return d.id; }
  if (inc.find('.') == std::string::npos) { kind = "std"; return {}; }  // <vector>, <string>, <cstdio> ...
  kind = "unknown";
  return {};
}

bool discoverProject(const fs::path& rootIn, const DiscoverOptions& opt, ProjectInfo& P, std::string& err) {
  std::error_code ec;
  if (!fs::is_directory(rootIn, ec)) {
    err = "not a directory: " + pathStr(rootIn);
    return false;
  }
  P.root = fs::weakly_canonical(fs::absolute(rootIn, ec), ec);
  if (ec) P.root = fs::absolute(rootIn);
  P.name = sanitizeName(P.root.filename().string());

  std::vector<std::string> srcRel, hdrRel;
  std::set<std::string> bs;
  std::set<fs::path> skip;
  for (const auto& s : opt.skipPaths) {
    std::error_code e;
    skip.insert(fs::weakly_canonical(s, e));
  }
  fs::recursive_directory_iterator it(P.root, fs::directory_options::skip_permission_denied, ec), end;
  if (ec) {
    err = "cannot read directory: " + ec.message();
    return false;
  }
  while (it != end) {
    const fs::path p = it->path();
    std::error_code e2;
    const bool link = it->is_symlink(e2);
    if (!link && it->is_directory(e2)) {
      std::error_code e3;
      if (skipDirName(p.filename().string()) || skip.count(fs::weakly_canonical(p, e3))) it.disable_recursion_pending();
    } else if (!link && it->is_regular_file(e2)) {
      if (++P.filesScanned > opt.maxFiles) {
        P.truncated = true;
        P.warnings.push_back("file limit reached; the project was only partially scanned");
        break;
      }
      const std::string name = p.filename().string();
      const std::string ext = lower(p.extension().string());
      const std::string rel = pathStr(p.lexically_relative(P.root));
      if (name == "CMakeLists.txt") bs.insert("cmake");
      else if (name == "Makefile" || name == "makefile" || name == "GNUmakefile") bs.insert("make");
      else if (name == "configure.ac" || name == "configure.in" || name == "Makefile.am" || name == "configure") bs.insert("autotools");
      else if (name == "meson.build") bs.insert("meson");
      if (ext == ".c") srcRel.push_back(rel);
      else if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".c++") srcRel.push_back(rel);
      else if (ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx") hdrRel.push_back(rel);
      else if (!junkFile(name, ext)) P.otherFiles.push_back(rel);
    }
    it.increment(ec);
    if (ec) break;
  }
  P.buildSystems.assign(bs.begin(), bs.end());
  std::sort(srcRel.begin(), srcRel.end());
  std::sort(P.otherFiles.begin(), P.otherFiles.end());
  P.headers.insert(hdrRel.begin(), hdrRel.end());

  // Candidate -I directories: every ancestor directory of every header, shallow first.
  std::set<std::string> dirSet = {""};
  for (const auto& h : hdrRel)
    for (std::string d = parentDir(h); !d.empty(); d = parentDir(d)) dirSet.insert(d);
  std::vector<std::string> incDirs(dirSet.begin(), dirSet.end());
  std::stable_sort(incDirs.begin(), incDirs.end(), [](const std::string& a, const std::string& b) {
    return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
  });

  auto scanOne = [&](const std::string& rel, bool isSource) {
    auto txt = readFile(P.root / fromUtf8(rel), 8u << 20);
    if (!txt) {
      P.warnings.push_back("could not read (or file larger than 8 MB): " + rel);
      return;
    }
    Scan sc = scanText(*txt);
    for (auto& lit : sc.literals) P.referencedNames.insert(lit);
    if (isSource) {
      const std::string ext = lower(fs::path(rel).extension().string());
      SourceFile sf;
      sf.rel = rel;
      sf.cxx = ext != ".c";
      sf.hasMain = sc.hasMain;
      (sf.cxx ? P.hasCxx : P.hasC) = true;
      P.sources.push_back(sf);
    }
    const std::string fromDir = parentDir(rel);
    std::set<std::string> direct;
    for (const auto& [inc, quoted] : sc.includes) {
      std::string found;
      if (quoted) {
        const std::string c = normalizeRel(fromDir, inc);
        if (!startsWith(c, "../") && P.headers.count(c)) found = c;
      }
      if (found.empty()) {
        for (const auto& d : incDirs) {
          const std::string c = normalizeRel(d, inc);
          if (!startsWith(c, "../") && P.headers.count(c)) {
            found = c;
            P.usedIncludeDirs.insert(d.empty() ? "." : d);
            break;
          }
        }
      }
      if (!found.empty()) {
        direct.insert(found);
        continue;
      }
      std::string kind;
      const std::string dep = classifyExternalHeader(inc, kind);
      if (kind == "std" || kind == "posix") continue;
      if (kind == "dep") {
        P.depFiles[dep].insert(rel);
        P.depHeaders[dep].insert(inc);
      } else if (quoted) {
        P.missing[inc].insert(rel);
      } else {
        P.unknownExternal.insert(inc);
      }
    }
    P.includes[rel].assign(direct.begin(), direct.end());
  };
  for (const auto& h : hdrRel) scanOne(h, false);
  for (const auto& s : srcRel) scanOne(s, true);
  std::sort(P.sources.begin(), P.sources.end(), [](const SourceFile& a, const SourceFile& b) { return a.rel < b.rel; });
  return true;
}

std::set<std::string> transitiveHeaders(const ProjectInfo& p, const std::string& rel) {
  std::set<std::string> seen;
  std::vector<std::string> stack = {rel};
  while (!stack.empty()) {
    const std::string cur = stack.back();
    stack.pop_back();
    auto it = p.includes.find(cur);
    if (it == p.includes.end()) continue;
    for (const auto& h : it->second)
      if (h != rel && seen.insert(h).second) stack.push_back(h);
  }
  return seen;
}

}  // namespace wf
