#pragma once
// Small, dependency-free helpers shared by every WasmForge module.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wf {
namespace fs = std::filesystem;

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string lower(std::string s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);

// Reads a whole file (works for /proc files too). Returns nullopt on error or if larger than maxBytes.
std::optional<std::string> readFile(const fs::path& p, std::size_t maxBytes = 256u << 20);
// Writes via temp file + rename so readers never see a half-written file.
bool writeFileAtomic(const fs::path& p, const std::string& data);

std::string humanBytes(std::uint64_t bytes);
std::string jsonEscape(const std::string& s);

// FNV-1a 64-bit: fast change detection for build caching. NOT a cryptographic hash.
std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t seed = 1469598103934665603ULL);
std::uint64_t fnv1a(const std::string& s, std::uint64_t seed = 1469598103934665603ULL);
std::string hex64(std::uint64_t v);
bool hashFile(const fs::path& p, std::uint64_t& out);

// True when `candidate` resolves to `root` or something under it (path-traversal guard).
bool isInside(const fs::path& root, const fs::path& candidate);
// UTF-8 <-> path conversion with forward slashes (stable across Windows/POSIX).
std::string pathStr(const fs::path& p);
fs::path fromUtf8(const std::string& s);

// Simple wildcard match: '*' = any run of characters, '?' = any single character.
bool globMatch(const std::string& pattern, const std::string& text);
double median(std::vector<double> v);
}  // namespace wf
