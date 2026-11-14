#include "assets/assets.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace wf {

const char* toString(AssetKind k) {
  switch (k) {
    case AssetKind::Wad: return "wad";
    case AssetKind::Image: return "image";
    case AssetKind::Audio: return "audio";
    case AssetKind::Midi: return "midi";
    case AssetKind::Json: return "json";
    case AssetKind::Text: return "text";
    case AssetKind::Css: return "css";
    case AssetKind::JavaScript: return "javascript";
    case AssetKind::Html: return "html";
    case AssetKind::Wasm: return "wasm";
    case AssetKind::StaticLib: return "static-lib";
    case AssetKind::SharedLib: return "shared-lib";
    case AssetKind::NativeBinary: return "native-binary";
    case AssetKind::Document: return "document";
    case AssetKind::BuildFile: return "build-file";
    case AssetKind::Binary: return "binary";
    default: return "unknown";
  }
}
const char* toString(AssetAction a) {
  switch (a) {
    case AssetAction::Embed: return "EMBED";
    case AssetAction::External: return "EXTERNAL";
    case AssetAction::Link: return "LINK";
    default: return "SKIP";
  }
}

namespace {
bool has(const std::string& h, std::size_t off, const char* lit, std::size_t n) {
  return h.size() >= off + n && h.compare(off, n, lit, n) == 0;
}

bool detectSignature(const std::string& h, AssetKind& k, std::string& label) {
  auto byte = [&](std::size_t i) { return i < h.size() ? static_cast<unsigned char>(h[i]) : 0u; };
  if (has(h, 0, "IWAD", 4)) { k = AssetKind::Wad; label = "IWAD"; return true; }
  if (has(h, 0, "PWAD", 4)) { k = AssetKind::Wad; label = "PWAD"; return true; }
  if (byte(0) == 0x89 && has(h, 1, "PNG", 3)) { k = AssetKind::Image; label = "PNG"; return true; }
  if (byte(0) == 0xFF && byte(1) == 0xD8 && byte(2) == 0xFF) { k = AssetKind::Image; label = "JPEG"; return true; }
  if (has(h, 0, "GIF8", 4)) { k = AssetKind::Image; label = "GIF"; return true; }
  if (has(h, 0, "RIFF", 4) && has(h, 8, "WAVE", 4)) { k = AssetKind::Audio; label = "WAV"; return true; }
  if (has(h, 0, "RIFF", 4) && has(h, 8, "WEBP", 4)) { k = AssetKind::Image; label = "WebP"; return true; }
  if (has(h, 0, "OggS", 4)) { k = AssetKind::Audio; label = "OGG"; return true; }
  if (has(h, 0, "fLaC", 4)) { k = AssetKind::Audio; label = "FLAC"; return true; }
  if (has(h, 0, "ID3", 3) || (byte(0) == 0xFF && (byte(1) == 0xFB || byte(1) == 0xFA || byte(1) == 0xF3 || byte(1) == 0xF2))) {
    k = AssetKind::Audio; label = "MP3"; return true;
  }
  if (has(h, 0, "MThd", 4)) { k = AssetKind::Midi; label = "MIDI"; return true; }
  if (byte(0) == 0 && has(h, 1, "asm", 3)) { k = AssetKind::Wasm; label = "WebAssembly"; return true; }
  if (has(h, 0, "!<arch>\n", 8)) { k = AssetKind::StaticLib; label = "ar archive"; return true; }
  if (byte(0) == 0x7F && has(h, 1, "ELF", 3)) { k = AssetKind::NativeBinary; label = "ELF"; return true; }
  if (has(h, 0, "MZ", 2)) { k = AssetKind::NativeBinary; label = "PE/COFF"; return true; }
  return false;
}

AssetKind kindFromExtension(const std::string& ext, const std::string& name) {
  const std::string n = lower(name);
  if (startsWith(n, "readme") || startsWith(n, "license") || startsWith(n, "licence") || startsWith(n, "copying") ||
      startsWith(n, "changelog") || startsWith(n, "authors") || n == "news" || ext == ".md" || ext == ".rst" || ext == ".1" || ext == ".5")
    return AssetKind::Document;
  if (n == "cmakelists.txt" || n == "configure" || startsWith(n, "makefile") || ext == ".cmake" || ext == ".in" || ext == ".am" ||
      ext == ".ac" || ext == ".gitignore" || n == ".gitignore" || n == ".gitattributes" || ext == ".sh" || ext == ".py" || ext == ".bat" || ext == ".ps1" || ext == ".yml" || ext == ".yaml")
    return AssetKind::BuildFile;
  if (ext == ".wad") return AssetKind::Wad;
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".bmp" || ext == ".webp" || ext == ".ico") return AssetKind::Image;
  if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") return AssetKind::Audio;
  if (ext == ".mid" || ext == ".midi") return AssetKind::Midi;
  if (ext == ".json") return AssetKind::Json;
  if (ext == ".txt" || ext == ".cfg" || ext == ".ini" || ext == ".conf") return AssetKind::Text;
  if (ext == ".css") return AssetKind::Css;
  if (ext == ".js" || ext == ".mjs") return AssetKind::JavaScript;
  if (ext == ".html" || ext == ".htm") return AssetKind::Html;
  if (ext == ".wasm") return AssetKind::Wasm;
  if (ext == ".a" || ext == ".lib") return AssetKind::StaticLib;
  if (ext == ".so" || ext == ".dll" || ext == ".dylib") return AssetKind::SharedLib;
  return AssetKind::Unknown;
}

bool looksLikeText(const std::string& h) {
  if (h.empty()) return true;
  std::size_t printable = 0;
  for (unsigned char c : h) {
    if (c == 0) return false;
    if (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127) || c >= 128) ++printable;
  }
  return printable * 100 >= h.size() * 95;
}

// Peeks at the first archive members to tell wasm objects apart from native ELF objects.
std::string inspectArchive(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "unknown";
  char magic[8];
  f.read(magic, 8);
  if (f.gcount() != 8) return "unknown";
  for (int member = 0; member < 8; ++member) {
    char hdr[60];
    f.read(hdr, 60);
    if (f.gcount() != 60) break;
    const std::string name(hdr, 16);
    const long size = std::atol(std::string(hdr + 48, 10).c_str());
    if (size < 0) break;
    const bool special = name[0] == '/' || startsWith(name, "__.SYMDEF");
    if (!special && size >= 4) {
      char head[4];
      f.read(head, 4);
      if (f.gcount() != 4) break;
      if (head[0] == 0 && head[1] == 'a' && head[2] == 's' && head[3] == 'm') return "wasm";
      if (head[0] == 0x7F && head[1] == 'E') return "native";
      if (head[0] == 'B' && head[1] == 'C' && static_cast<unsigned char>(head[2]) == 0xC0) return "bitcode";
      return "unknown";
    }
    f.seekg(size + (size & 1) - 0, std::ios::cur);
    if (!f) break;
  }
  return "unknown";
}
}  // namespace

AssetInfo classifyAsset(const fs::path& root, const std::string& rel) {
  AssetInfo a;
  a.rel = rel;
  const fs::path abs = root / fromUtf8(rel);
  std::error_code ec;
  a.size = static_cast<std::uint64_t>(fs::file_size(abs, ec));
  if (ec) a.size = 0;
  std::string head;
  {
    std::ifstream f(abs, std::ios::binary);
    char buf[512];
    f.read(buf, sizeof buf);
    head.assign(buf, static_cast<std::size_t>(f.gcount()));
  }
  const std::string name = abs.filename().string();
  const std::string ext = lower(abs.extension().string());
  const AssetKind byExt = kindFromExtension(ext, name);

  AssetKind bySig;
  std::string label;
  if (a.size > 0 && detectSignature(head, bySig, label)) {
    a.kind = bySig;
    a.detectedBy = "signature";
    a.signature = label;
    if (byExt != AssetKind::Unknown && byExt != bySig && byExt != AssetKind::Document && byExt != AssetKind::BuildFile &&
        !(byExt == AssetKind::SharedLib && bySig == AssetKind::NativeBinary) && !(byExt == AssetKind::Image && bySig == AssetKind::Image) &&
        !(byExt == AssetKind::Audio && bySig == AssetKind::Audio))
      a.note = std::string("extension '") + ext + "' says " + toString(byExt) + " but the file signature is " + label;
    if (byExt == AssetKind::SharedLib && bySig == AssetKind::NativeBinary) a.kind = AssetKind::SharedLib;
    if (byExt == AssetKind::Document || byExt == AssetKind::BuildFile) a.kind = byExt;  // names win over a lucky signature
  } else {
    a.kind = byExt;
    a.detectedBy = "extension";
    if (byExt == AssetKind::Unknown) {
      a.kind = looksLikeText(head) ? AssetKind::Text : AssetKind::Binary;
      a.detectedBy = "content";
    }
    if (byExt == AssetKind::Wad && a.size > 0) a.note = "no IWAD/PWAD signature: the file may be corrupted or not a WAD";
    if ((byExt == AssetKind::Image || byExt == AssetKind::Audio || byExt == AssetKind::Midi) && a.size > 0 && ext != ".bmp" && ext != ".ico")
      a.note = "extension says " + std::string(toString(byExt)) + " but no known signature was found";
  }
  if (a.kind == AssetKind::StaticLib) a.archiveContent = inspectArchive(abs);
  return a;
}

void decideAssets(std::vector<AssetInfo>& assets, AssetMode mode, const std::set<std::string>& referencedNames,
                  const std::vector<std::string>& forceGlobs) {
  for (auto& a : assets) {
    const std::string base = lower(fs::path(a.rel).filename().string());
    a.referenced = referencedNames.count(base) > 0;
    bool forced = false;
    for (const auto& g : forceGlobs)
      if (globMatch(g, a.rel) || globMatch(g, fs::path(a.rel).filename().string())) forced = true;
    const bool dataLike = a.kind == AssetKind::Wad || a.kind == AssetKind::Image || a.kind == AssetKind::Audio ||
                          a.kind == AssetKind::Midi || a.kind == AssetKind::Json || a.kind == AssetKind::Text ||
                          a.kind == AssetKind::Css || a.kind == AssetKind::JavaScript || a.kind == AssetKind::Html ||
                          a.kind == AssetKind::Wasm || a.kind == AssetKind::Binary;
    switch (a.kind) {
      case AssetKind::Document: a.action = AssetAction::Skip; a.reason = "documentation, not needed at runtime"; continue;
      case AssetKind::BuildFile: a.action = AssetAction::Skip; a.reason = "build/tooling file, not a runtime resource"; continue;
      case AssetKind::SharedLib:
      case AssetKind::NativeBinary:
        a.action = AssetAction::Skip;
        a.reason = "native " + std::string(a.signature.empty() ? "library" : a.signature) + " cannot be used by WebAssembly; the project must be rebuilt from source";
        continue;
      case AssetKind::StaticLib:
        if (a.archiveContent == "wasm" || a.archiveContent == "bitcode") {
          a.action = AssetAction::Link;
          a.reason = "archive contains " + a.archiveContent + " objects: added to the link";
        } else {
          a.action = AssetAction::Skip;
          a.reason = a.archiveContent == "native" ? "archive contains native objects (not WebAssembly): not linked" : "archive contents could not be identified: not linked";
        }
        continue;
      default: break;
    }
    if (!dataLike) { a.action = AssetAction::Skip; a.reason = "not a recognised runtime resource"; continue; }
    if (!a.referenced && !forced) {
      a.action = AssetAction::Skip;
      a.reason = "file name does not appear in any source string literal (force it with --asset <glob>)";
      continue;
    }
    const std::string why = forced ? "forced by --asset" : "referenced by name in the sources";
    if (mode == AssetMode::Embed) { a.action = AssetAction::Embed; a.reason = why + "; assets=embed"; continue; }
    if (mode == AssetMode::External || mode == AssetMode::Preserve) {
      a.action = AssetAction::External;
      a.reason = why + "; assets=" + toString(mode) + ": original bytes shipped next to the module";
      continue;
    }
    if (a.kind != AssetKind::Wad && a.size <= 32 * 1024) {
      a.action = AssetAction::Embed;
      a.reason = why + "; small (" + humanBytes(a.size) + "): embedded unchanged";
    } else {
      a.action = AssetAction::External;
      a.reason = why + "; " + (a.kind == AssetKind::Wad ? "game data" : "large (" + humanBytes(a.size) + ")") + ": kept in its original format and mounted into the virtual filesystem";
    }
  }
}

std::vector<AssetInfo> planAssets(const ProjectInfo& proj, AssetMode mode, const std::vector<std::string>& forceGlobs) {
  std::vector<AssetInfo> out;
  out.reserve(proj.otherFiles.size());
  for (const auto& rel : proj.otherFiles) out.push_back(classifyAsset(proj.root, rel));
  decideAssets(out, mode, proj.referencedNames, forceGlobs);
  return out;
}
}  // namespace wf
