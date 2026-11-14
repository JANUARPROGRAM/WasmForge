#include "core/util.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace wf {

std::string trim(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool startsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::optional<std::string> readFile(const fs::path& p, std::size_t maxBytes) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::nullopt;
  std::string data;
  char buf[65536];
  for (;;) {
    f.read(buf, sizeof buf);
    const std::streamsize n = f.gcount();
    if (n <= 0) break;
    data.append(buf, static_cast<std::size_t>(n));
    if (data.size() > maxBytes) return std::nullopt;
  }
  return data;
}

bool writeFileAtomic(const fs::path& p, const std::string& data) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  static std::atomic<unsigned> counter{0};
  fs::path tmp = p;
  tmp += ".tmp" + std::to_string(counter++) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffff);
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) {
      f.close();
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::string humanBytes(std::uint64_t b) {
  static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(b);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
  else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
  return buf;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o.push_back(static_cast<char>(c));
        }
    }
  }
  return o;
}

std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t h) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}
std::uint64_t fnv1a(const std::string& s, std::uint64_t seed) { return fnv1a(s.data(), s.size(), seed); }

std::string hex64(std::uint64_t v) {
  char b[20];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

bool hashFile(const fs::path& p, std::uint64_t& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::uint64_t h = 1469598103934665603ULL;
  char buf[65536];
  for (;;) {
    f.read(buf, sizeof buf);
    const std::streamsize n = f.gcount();
    if (n <= 0) break;
    h = fnv1a(buf, static_cast<std::size_t>(n), h);
  }
  out = h;
  return true;
}

bool isInside(const fs::path& root, const fs::path& candidate) {
  std::error_code ec;
  const fs::path r = fs::weakly_canonical(root, ec);
  if (ec) return false;
  const fs::path c = fs::weakly_canonical(candidate, ec);
  if (ec) return false;
  const fs::path rel = c.lexically_relative(r);
  if (rel.empty()) return false;
  return *rel.begin() != "..";
}

std::string pathStr(const fs::path& p) { return p.generic_u8string(); }
fs::path fromUtf8(const std::string& s) { return fs::u8path(s); }

bool globMatch(const std::string& pat, const std::string& text) {
  std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
  while (t < text.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

}  // namespace wf
