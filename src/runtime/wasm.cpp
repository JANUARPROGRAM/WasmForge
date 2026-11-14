#include "runtime/wasm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>

#include "core/util.hpp"

namespace wf {
namespace {

struct Reader {
  const std::string& d;
  std::size_t pos;
  std::size_t end;
  bool ok = true;

  Reader(const std::string& data, std::size_t p, std::size_t e) : d(data), pos(p), end(e) {}
  bool eof() const { return pos >= end; }

  std::uint8_t u8() {
    if (pos >= end) { ok = false; return 0; }
    return static_cast<std::uint8_t>(d[pos++]);
  }
  // LEB128, at most `maxBytes` bytes (5 for u32, 10 for u64): rejects over-long encodings.
  std::uint64_t leb(int maxBytes) {
    std::uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < maxBytes; ++i) {
      const std::uint8_t b = u8();
      if (!ok) return 0;
      result |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return result;
      shift += 7;
    }
    ok = false;
    return 0;
  }
  std::uint32_t u32() { return static_cast<std::uint32_t>(leb(5)); }
  std::uint64_t u64() { return leb(10); }
  void skipLeb(int maxBytes) { leb(maxBytes); }
  std::string name() {
    const std::uint32_t n = u32();
    if (!ok || n > end - pos) { ok = false; return {}; }
    std::string s = d.substr(pos, n);
    pos += n;
    return s;
  }
  void skip(std::uint64_t n) {
    if (n > end - pos) { ok = false; return; }
    pos += static_cast<std::size_t>(n);
  }
};

const char* kindName(std::uint8_t k) {
  switch (k) {
    case 0: return "func";
    case 1: return "table";
    case 2: return "memory";
    case 3: return "global";
    case 4: return "tag";
    default: return "unknown";
  }
}

const char* sectionName(std::uint8_t id) {
  static const char* names[] = {"custom", "type", "import", "function", "table", "memory", "global", "export", "start", "element", "code", "data", "datacount", "tag"};
  return id < 14 ? names[id] : "unknown";
}

struct Limits {
  std::uint64_t min = 0, max = 0;
  bool hasMax = false, shared = false, is64 = false;
};
Limits readLimits(Reader& r) {
  Limits l;
  const std::uint8_t flags = r.u8();
  l.hasMax = flags & 1;
  l.shared = flags & 2;
  l.is64 = flags & 4;
  l.min = l.is64 ? r.u64() : r.u32();
  if (l.hasMax) l.max = l.is64 ? r.u64() : r.u32();
  return l;
}

// Skips a constant init expression (the forms real toolchains emit) up to and including `end`.
bool skipConstExpr(Reader& r) {
  const std::uint8_t op = r.u8();
  switch (op) {
    case 0x41: r.skipLeb(5); break;   // i32.const
    case 0x42: r.skipLeb(10); break;  // i64.const
    case 0x43: r.skip(4); break;      // f32.const
    case 0x44: r.skip(8); break;      // f64.const
    case 0x23: r.skipLeb(5); break;   // global.get
    default: return false;
  }
  return r.ok && r.u8() == 0x0B && r.ok;
}
}  // namespace

std::set<std::string> WasmInfo::importModules() const {
  std::set<std::string> s;
  for (const auto& i : imports) s.insert(i.module);
  return s;
}
bool WasmInfo::importsWasi() const {
  for (const auto& i : imports)
    if (startsWith(i.module, "wasi_snapshot_") || i.module == "wasi_unstable" || startsWith(i.module, "wasi:")) return true;
  return false;
}
std::uint64_t WasmInfo::sectionBytes(const std::string& name) const {
  std::uint64_t t = 0;
  for (const auto& s : sections)
    if (s.name == name) t += s.bytes;
  return t;
}

WasmInfo parseWasm(const std::string& d) {
  WasmInfo w;
  w.fileBytes = d.size();
  if (d.size() < 8) { w.error = "file is shorter than the 8-byte WebAssembly header"; return w; }
  static const char magic[4] = {0, 'a', 's', 'm'};
  if (d.compare(0, 4, magic, 4) != 0) { w.error = "bad magic number (not a WebAssembly binary)"; return w; }
  w.version = static_cast<std::uint8_t>(d[4]) | (static_cast<std::uint8_t>(d[5]) << 8) | (static_cast<std::uint8_t>(d[6]) << 16) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[7])) << 24);
  if (w.version != 1) { w.error = "unsupported WebAssembly version " + std::to_string(w.version); return w; }

  std::size_t pos = 8;
  while (pos < d.size()) {
    Reader hdr(d, pos, d.size());
    const std::uint8_t id = hdr.u8();
    const std::uint32_t size = hdr.u32();
    if (!hdr.ok) { w.error = "truncated section header at offset " + std::to_string(pos); return w; }
    if (size > d.size() - hdr.pos) { w.error = std::string("section '") + sectionName(id) + "' claims " + std::to_string(size) + " bytes but the file ends first (truncated or corrupt)"; return w; }
    const std::size_t start = hdr.pos, endp = hdr.pos + size;
    Reader r(d, start, endp);
    std::string secName = sectionName(id);
    if (id == 0) {
      const std::string cn = r.name();
      secName = "custom:" + (r.ok ? cn : std::string("?"));
    } else if (id > 13) {
      w.error = "unknown section id " + std::to_string(id);
      return w;
    }
    w.sections.push_back({secName, static_cast<std::uint64_t>(size) + (hdr.pos - pos)});

    bool parsed = true;
    switch (id) {
      case 2: {  // import
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n && r.ok; ++i) {
          WasmImport im;
          im.module = r.name();
          im.name = r.name();
          const std::uint8_t kind = r.u8();
          im.kind = kindName(kind);
          switch (kind) {
            case 0: r.skipLeb(5); ++w.functionsImported; break;
            case 1: r.u8(); readLimits(r); break;
            case 2: {
              const Limits l = readLimits(r);
              w.hasMemory = w.memoryImported = true;
              w.memoryMinPages = l.min; w.memoryMaxPages = l.max; w.memoryHasMax = l.hasMax; w.memoryShared = l.shared; w.memory64 = l.is64;
              break;
            }
            case 3: r.u8(); r.u8(); break;
            case 4: r.u8(); r.skipLeb(5); break;
            default: r.ok = false;
          }
          w.imports.push_back(im);
        }
        break;
      }
      case 3: w.functionsDefined = r.u32(); break;
      case 5: {  // memory
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n && r.ok; ++i) {
          const Limits l = readLimits(r);
          if (i == 0) {
            w.hasMemory = true;
            w.memoryMinPages = l.min; w.memoryMaxPages = l.max; w.memoryHasMax = l.hasMax; w.memoryShared = l.shared; w.memory64 = l.is64;
          }
        }
        break;
      }
      case 7: {  // export
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n && r.ok; ++i) {
          WasmExport e;
          e.name = r.name();
          e.kind = kindName(r.u8());
          r.skipLeb(5);
          w.exports.push_back(e);
        }
        break;
      }
      case 8: w.hasStart = true; break;
      case 10: w.codeBytes += size; break;
      case 11: {  // data
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n && r.ok; ++i) {
          const std::uint32_t flag = r.u32();
          if (flag == 2) r.skipLeb(5);
          if (flag == 0 || flag == 2) {
            if (!skipConstExpr(r)) { if (r.ok) w.dataFullyParsed = false; parsed = false; break; }
          } else if (flag != 1) {
            r.ok = false;
            break;
          }
          const std::uint32_t len = r.u32();
          r.skip(len);
          w.dataBytes += len;
          ++w.dataSegments;
        }
        break;
      }
      default: break;  // other sections are accounted for by size only
    }
    if (!r.ok) { w.error = "malformed contents in section '" + secName + "'"; return w; }
    (void)parsed;
    pos = endp;
  }
  w.valid = true;
  return w;
}

std::string classifyWasmTarget(const WasmInfo& w) {
  if (w.importsWasi()) return "wasi";
  if (w.imports.empty()) return "standalone";
  for (const auto& i : w.imports)
    if (i.module == "env" && (startsWith(i.name, "emscripten_") || startsWith(i.name, "_emscripten_") || startsWith(i.name, "__syscall_") || startsWith(i.name, "invoke_")))
      return "emscripten";
  return "host-imports";
}

std::string describeWasm(const WasmInfo& w) {
  std::ostringstream o;
  if (!w.valid) { o << "  INVALID: " << w.error << "\n"; return o.str(); }
  auto pct = [&](std::uint64_t b) { return w.fileBytes ? static_cast<int>(b * 100 / w.fileBytes) : 0; };
  o << "  size          " << humanBytes(w.fileBytes) << " (wasm version " << w.version << ")\n";
  o << "  target        " << classifyWasmTarget(w) << "  (" << w.imports.size() << " imports from " << w.importModules().size() << " module(s), " << w.exports.size() << " exports)\n";
  o << "  functions     " << w.functionsDefined << " defined, " << w.functionsImported << " imported" << (w.hasStart ? ", has start function" : "") << "\n";
  if (w.hasMemory) {
    o << "  memory        " << humanBytes(w.memoryMinPages * 65536) << " initial, " << (w.memoryHasMax ? humanBytes(w.memoryMaxPages * 65536) + " max" : "no maximum")
      << (w.memoryImported ? ", imported" : ", defined by module") << (w.memoryShared ? ", SHARED" : "") << (w.memory64 ? ", memory64" : "") << "\n";
  }
  o << "  code / data   " << humanBytes(w.codeBytes) << " (" << pct(w.codeBytes) << "%) / " << humanBytes(w.dataBytes) << " in " << w.dataSegments << " segment(s) (" << pct(w.dataBytes) << "%)\n";
  std::vector<WasmSection> top = w.sections;
  std::sort(top.begin(), top.end(), [](const WasmSection& a, const WasmSection& b) { return a.bytes > b.bytes; });
  o << "  largest       ";
  for (std::size_t i = 0; i < top.size() && i < 4; ++i) o << (i ? ", " : "") << top[i].name << " " << humanBytes(top[i].bytes);
  o << "\n";
  return o.str();
}

namespace {
std::string signedBytes(long long d) {
  if (d == 0) return "0 B";
  return std::string(d > 0 ? "+" : "-") + humanBytes(static_cast<std::uint64_t>(d > 0 ? d : -d));
}
std::string percent(std::uint64_t before, std::uint64_t after) {
  if (before == 0) return after == 0 ? "0%" : "new";
  char b[32];
  std::snprintf(b, sizeof b, "%+.1f%%", (static_cast<double>(after) - static_cast<double>(before)) * 100.0 / static_cast<double>(before));
  return b;
}
}  // namespace

std::string diffWasm(const WasmInfo& a, const WasmInfo& b) {
  std::ostringstream o;
  if (!a.valid || !b.valid) {
    o << "  cannot compare: " << (!a.valid ? "first" : "second") << " file is not a valid WebAssembly binary (" << (!a.valid ? a.error : b.error) << ")\n";
    return o.str();
  }
  o << "  total size    " << humanBytes(a.fileBytes) << " -> " << humanBytes(b.fileBytes) << "  (" << signedBytes(static_cast<long long>(b.fileBytes) - static_cast<long long>(a.fileBytes)) << ", " << percent(a.fileBytes, b.fileBytes) << ")\n";
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> sec;
  for (const auto& s : a.sections) sec[s.name].first += s.bytes;
  for (const auto& s : b.sections) sec[s.name].second += s.bytes;
  std::vector<std::pair<std::string, long long>> deltas;
  for (const auto& [n, p] : sec)
    if (p.first != p.second) deltas.emplace_back(n, static_cast<long long>(p.second) - static_cast<long long>(p.first));
  std::sort(deltas.begin(), deltas.end(), [](const auto& x, const auto& y) { return std::llabs(x.second) > std::llabs(y.second); });
  if (deltas.empty()) o << "  sections      identical sizes\n";
  for (const auto& [n, d] : deltas) {
    const auto& p = sec[n];
    o << "  " << std::left << std::setw(14) << n << humanBytes(p.first) << " -> " << humanBytes(p.second) << "  (" << signedBytes(d) << ", " << percent(p.first, p.second) << ")\n";
  }
  auto key = [](const WasmImport& i) { return i.module + "." + i.name + " [" + i.kind + "]"; };
  std::set<std::string> ia, ib, ea, eb;
  for (const auto& i : a.imports) ia.insert(key(i));
  for (const auto& i : b.imports) ib.insert(key(i));
  for (const auto& e : a.exports) ea.insert(e.name + " [" + e.kind + "]");
  for (const auto& e : b.exports) eb.insert(e.name + " [" + e.kind + "]");
  auto listDiff = [&](const char* what, const std::set<std::string>& x, const std::set<std::string>& y) {
    std::size_t added = 0, removed = 0;
    std::ostringstream detail;
    for (const auto& k : y) if (!x.count(k)) { if (added++ < 6) detail << "      + " << k << "\n"; }
    for (const auto& k : x) if (!y.count(k)) { if (removed++ < 6) detail << "      - " << k << "\n"; }
    if (added || removed) o << "  " << what << " +" << added << " -" << removed << "\n" << detail.str();
  };
  listDiff("imports      ", ia, ib);
  listDiff("exports      ", ea, eb);
  if (a.memoryMinPages != b.memoryMinPages)
    o << "  memory        " << humanBytes(a.memoryMinPages * 65536) << " -> " << humanBytes(b.memoryMinPages * 65536) << " initial\n";
  if (classifyWasmTarget(a) != classifyWasmTarget(b)) o << "  target        " << classifyWasmTarget(a) << " -> " << classifyWasmTarget(b) << "\n";
  return o.str();
}

std::vector<Diagnostic> wasmDiagnostics(const WasmInfo& w, bool webTarget) {
  std::vector<Diagnostic> d;
  if (!w.valid) {
    d.push_back({Level::Error, "E-WASM-INVALID", "invalid WebAssembly binary: " + w.error, "The backend produced a corrupt file; rebuild with --force."});
    return d;
  }
  if (webTarget && w.importsWasi())
    d.push_back({Level::Error, "E-WASM-WASI-IN-WEB", "the module imports WASI functions, which browsers do not provide",
                 "Use the emscripten backend for browser output, or run this module with a WASI runtime (wasmtime, wasmer, node:wasi)."});
  if (w.memoryShared)
    d.push_back({Level::Warning, "W-WASM-SHARED-MEMORY", "the module uses shared memory (threads)",
                 "Browsers only allow this on cross-origin-isolated pages: serve with `wasmfrg serve --isolate`."});
  if (w.hasMemory && !w.memoryImported && w.memoryMinPages * 65536ull > (256ull << 20))
    d.push_back({Level::Warning, "W-WASM-MEMORY", "initial memory is " + humanBytes(w.memoryMinPages * 65536) + "; low-end phones may fail to allocate it",
                 "Lower INITIAL_MEMORY and rely on memory growth."});
  if (w.fileBytes > 0 && w.codeBytes * 100 / w.fileBytes > 80 && w.fileBytes > (2u << 20))
    d.push_back({Level::Info, "I-WASM-CODE-HEAVY", "code is " + std::to_string(w.codeBytes * 100 / w.fileBytes) + "% of a " + humanBytes(w.fileBytes) + " module", "Try --strategy compact."});
  return d;
}
}  // namespace wf
