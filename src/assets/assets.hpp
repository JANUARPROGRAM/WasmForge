#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "core/util.hpp"

namespace wf {

enum class AssetKind { Wad, Image, Audio, Midi, Json, Text, Css, JavaScript, Html, Wasm, StaticLib, SharedLib, NativeBinary, Document, BuildFile, Binary, Unknown };
enum class AssetAction { Embed, External, Link, Skip };

const char* toString(AssetKind);
const char* toString(AssetAction);

struct AssetInfo {
  std::string rel;
  std::uint64_t size = 0;
  AssetKind kind = AssetKind::Unknown;
  std::string detectedBy;      // "signature" | "extension" | "content"
  std::string signature;       // human-readable signature name, if any
  std::string note;            // e.g. extension/signature mismatch
  std::string archiveContent;  // static libs: "wasm" | "native" | "bitcode" | "unknown"
  bool referenced = false;     // file name appears in a string literal of the sources
  AssetAction action = AssetAction::Skip;
  std::string reason;
};

// Classifies using file signature first (magic bytes), then extension, then content sniffing.
AssetInfo classifyAsset(const fs::path& root, const std::string& rel);
// Decides what to do with each asset. Original bytes are never modified (no TRANSFORM yet).
void decideAssets(std::vector<AssetInfo>& assets, AssetMode mode, const std::set<std::string>& referencedNames,
                  const std::vector<std::string>& forceGlobs);
std::vector<AssetInfo> planAssets(const ProjectInfo& proj, AssetMode mode, const std::vector<std::string>& forceGlobs);
}  // namespace wf
