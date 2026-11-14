#include "project/templates.hpp"

#include <algorithm>
#include <cctype>

namespace wf {
namespace {

bool idStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool idChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string ltrimStr(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

// Reads an identifier at the start of `s` (after blanks). Returns it and the remainder.
bool takeIdent(const std::string& s, std::string& id, std::string& rest) {
  const std::string t = ltrimStr(s);
  if (t.empty() || !idStart(t[0])) return false;
  std::size_t e = 1;
  while (e < t.size() && idChar(t[e])) ++e;
  id = t.substr(0, e);
  rest = t.substr(e);
  return true;
}

std::string normDep(const std::string& s) {
  std::string o;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (o.size() > 3 && o.compare(0, 3, "lib") == 0) o = o.substr(3);
  return o == "z" ? "zlib" : o;
}

bool looksNumeric(const std::string& v) {
  if (v.empty()) return false;
  std::size_t i = (v[0] == '-' ? 1 : 0);
  if (i >= v.size()) return false;
  for (; i < v.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
  return true;
}

// Autotools-style value: numbers and already-quoted strings pass through, anything else becomes a C string.
std::string cLiteral(const std::string& v) {
  if (looksNumeric(v) || (v.size() >= 2 && v.front() == '"' && v.back() == '"')) return v;
  std::string o = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\') o.push_back('\\');
    o.push_back(c);
  }
  return o + "\"";
}

const std::set<std::string>& guessedHeaders() {
  static const std::set<std::string> s = {"UNISTD_H", "STDINT_H", "INTTYPES_H", "STRING_H", "STRINGS_H", "STDLIB_H", "SYS_TYPES_H", "SYS_STAT_H",
                                          "SYS_TIME_H", "MEMORY_H", "DLFCN_H", "FCNTL_H", "DIRENT_H", "STDIO_H", "LIMITS_H", "ERRNO_H", "MATH_H",
                                          "CTYPE_H", "SIGNAL_H", "TIME_H", "SYS_PARAM_H", "SYS_MMAN_H", "SYS_SELECT_H", "NETINET_IN_H", "ARPA_INET_H",
                                          "NETDB_H", "PTHREAD_H", "SCHED_H", "POLL_H", "SYS_SOCKET_H"};
  return s;
}
const std::set<std::string>& guessedFunctions() {
  static const std::set<std::string> s = {"STRCASECMP", "STRNCASECMP", "STRDUP", "SNPRINTF", "VSNPRINTF", "MKDIR", "GETENV", "MEMMOVE", "MEMSET", "MEMCPY",
                                          "STRCHR", "STRRCHR", "STRSTR", "STRTOL", "STRTOUL", "ATEXIT", "QSORT", "GETTIMEOFDAY", "CLOCK_GETTIME",
                                          "USLEEP", "NANOSLEEP", "FSEEKO", "FTELLO", "SETENV", "UNSETENV", "REALPATH"};
  return s;
}
}  // namespace

bool isTruthy(const std::string& raw) {
  const std::string v = lower(trim(raw));
  if (v.empty() || v == "0" || v == "off" || v == "no" || v == "false" || v == "n" || v == "ignore" || v == "notfound") return false;
  return !endsWith(v, "-notfound");
}

std::string templateTargetName(const std::string& rel) {
  std::string base = rel.substr(rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1);
  for (const char* suf : {".cin", ".in", ".cmake"}) {
    if (endsWith(base, suf) && base.size() > std::string(suf).size()) {
      const std::string t = base.substr(0, base.size() - std::string(suf).size());
      for (const char* ext : {".h", ".hh", ".hpp", ".hxx"})
        if (endsWith(t, ext)) return t;
    }
  }
  return {};
}

bool parseVarAssignment(const std::string& text, std::string& name, std::string& value) {
  const auto eq = text.find('=');
  name = trim(eq == std::string::npos ? text : text.substr(0, eq));
  value = eq == std::string::npos ? "1" : text.substr(eq + 1);
  if (name.empty() || !idStart(name[0])) return false;
  for (char c : name)
    if (!idChar(c)) return false;
  return name.size() <= 128;
}

TemplateVar deriveVar(const std::string& name, const TemplateContext& ctx) {
  TemplateVar v;
  auto set = [&](const std::string& val, const std::string& origin, const std::string& note) {
    v.defined = true; v.value = val; v.origin = origin; v.note = note;
  };
  const std::string ln = lower(ctx.projectName);
  if (name == "PACKAGE_NAME" || name == "PROJECT_NAME") set(ctx.projectName, "placeholder", "project directory name; set the real one with --var " + name + "=...");
  else if (name == "PACKAGE_TARNAME" || name == "PACKAGE") set(ln, "placeholder", "lower-case project name");
  else if (name == "PACKAGE_VERSION" || name == "PROJECT_VERSION" || name == "VERSION") set("0.0.0", "placeholder", "unknown version; set with --var " + name + "=...");
  else if (name == "PACKAGE_STRING") set(ctx.projectName + " 0.0.0", "placeholder", "");
  else if (name == "PACKAGE_BUGREPORT" || name == "PACKAGE_URL") set("", "placeholder", "empty");
  else if (name == "WORDS_BIGENDIAN") { v.origin = "derived"; v.note = "WebAssembly is little-endian"; }
  else if (name == "STDC_HEADERS") set("1", "guess", "ISO C headers exist in the wasm libc");
  else if (startsWith(name, "HAVE_") && endsWith(name, "_H") && guessedHeaders().count(name.substr(5))) set("1", "guess", "assumed present in the wasm libc sysroot (not probed)");
  else if (startsWith(name, "HAVE_") && guessedFunctions().count(name.substr(5))) set("1", "guess", "assumed present in the wasm libc (not probed)");
  else if (startsWith(name, "HAVE_")) {
    const std::string key = normDep(name.substr(5));
    for (const auto& [dep, ok] : ctx.depSupported) {
      if (normDep(dep) != key) continue;
      if (ok) set("1", "derived", "a source includes its header and the backend can supply it");
      else { v.origin = "derived"; v.note = "used by the sources but the backend has no wasm port of it"; }
      return v;
    }
    v.origin = "unset";
    v.note = "not probed; define it with --var " + name + "=1 if needed";
  } else {
    v.origin = "unset";
    v.note = "not probed; define it with --var " + name + "=... if needed";
  }
  return v;
}

std::string renderTemplate(const std::string& text, const VarResolver& resolve, const std::string& sourceName, std::vector<VarUse>& uses) {
  std::map<std::string, VarUse> seen;
  auto record = [&](const std::string& name, const std::string& kind, const TemplateVar& v, bool enabled) {
    VarUse& u = seen[kind + ":" + name];
    u.name = name; u.kind = kind; u.var = v; u.enabled = enabled;
  };
  auto subst = [&](const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '@' && i + 1 < s.size() && idStart(s[i + 1])) {
        std::size_t e = i + 1;
        while (e < s.size() && idChar(s[e])) ++e;
        if (e < s.size() && s[e] == '@') {
          const std::string name = s.substr(i + 1, e - i - 1);
          const TemplateVar v = resolve(name);
          record(name, "subst", v, v.defined);
          o += v.defined ? v.value : "";  // CMake replaces unknown variables with an empty string
          i = e + 1;
          continue;
        }
      }
      o.push_back(s[i++]);
    }
    return o;
  };

  std::string out = "/* Generated by WasmForge from " + sourceName + ". Do not edit: re-run with --var NAME=VALUE instead. */\n";
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t e = text.find('\n', pos);
    const bool hasNl = e != std::string::npos;
    if (!hasNl) e = text.size();
    std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    std::string cr;
    if (!line.empty() && line.back() == '\r') { cr = "\r"; line.pop_back(); }
    const std::string t = ltrimStr(line);
    std::string id, rest;
    std::string result;
    if (startsWith(t, "#cmakedefine01") && takeIdent(t.substr(14), id, rest)) {
      const TemplateVar v = resolve(id);
      const bool on = v.defined && isTruthy(v.value);
      record(id, "cmakedefine01", v, on);
      result = "#define " + id + (on ? " 1" : " 0");
    } else if (startsWith(t, "#cmakedefine") && takeIdent(t.substr(12), id, rest)) {
      const TemplateVar v = resolve(id);
      const bool on = v.defined && isTruthy(v.value);
      record(id, "cmakedefine", v, on);
      const std::string tail = trim(subst(rest));
      result = on ? "#define " + id + (tail.empty() ? "" : " " + tail) : "/* #undef " + id + " */";
    } else if (startsWith(t, "#undef") && t.size() > 6 && (t[6] == ' ' || t[6] == '\t') && takeIdent(t.substr(6), id, rest)) {
      const TemplateVar v = resolve(id);
      const bool on = v.defined && isTruthy(v.value);
      record(id, "undef", v, on);
      result = on ? "#define " + id + " " + cLiteral(v.value) : "/* #undef " + id + " */";
    } else {
      result = subst(line);
    }
    out += result + cr + (hasNl ? "\n" : "");
  }
  for (auto& [k, u] : seen) uses.push_back(u);
  return out;
}

std::vector<GeneratedHeader> planGeneratedHeaders(const ProjectInfo& P, const TemplateContext& ctx, std::vector<Diagnostic>& diags) {
  std::vector<GeneratedHeader> result;
  for (const auto& [inc, files] : P.missing) {
    std::set<std::string> includers;
    for (const auto& f : files)
      if (ctx.activeFiles.count(f)) includers.insert(f);
    if (includers.empty()) continue;
    // Never let an include string choose where we write.
    bool unsafe = inc.empty() || inc[0] == '/' || inc.find("..") != std::string::npos || inc.find('\\') != std::string::npos || inc.find(':') != std::string::npos;
    if (unsafe) continue;
    const std::string base = inc.substr(inc.rfind('/') == std::string::npos ? 0 : inc.rfind('/') + 1);
    std::vector<std::string> candidates;
    for (const auto& rel : P.otherFiles)
      if (templateTargetName(rel) == base) candidates.push_back(rel);
    if (candidates.empty()) continue;
    if (candidates.size() > 1)
      diags.push_back({Level::Warning, "W-TEMPLATE-AMBIGUOUS", "several templates could produce \"" + inc + "\": using " + candidates.front(), "Remove the others or pass --exclude."});
    GeneratedHeader g;
    g.include = inc;
    g.templateRel = candidates.front();
    g.outRel = ".wasmforge/gen/include/" + inc;
    g.includers = includers;
    auto tpl = readFile(P.root / fromUtf8(g.templateRel), 2u << 20);
    if (!tpl) {
      diags.push_back({Level::Warning, "W-TEMPLATE-READ", "cannot read template " + g.templateRel, ""});
      continue;
    }
    VarResolver resolve = [&](const std::string& name) -> TemplateVar {
      auto it = ctx.user.find(name);
      if (it != ctx.user.end()) return it->second;
      return deriveVar(name, ctx);
    };
    g.content = renderTemplate(*tpl, resolve, g.templateRel, g.uses);
    g.hash = fnv1a(g.content);
    result.push_back(std::move(g));
  }
  return result;
}
}  // namespace wf
