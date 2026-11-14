#include "runtime/server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kBadSock = INVALID_SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kBadSock = -1;
static void closeSock(sock_t s) { ::close(s); }
#endif

#include "platform/platform.hpp"

namespace wf {
namespace {

std::atomic<bool> g_stop{false};
extern "C" void onSignal(int) { g_stop = true; }

constexpr std::size_t kMaxHead = 8192;
constexpr int kMaxConnections = 32;

bool sendAll(sock_t s, const char* p, std::size_t n) {
  while (n > 0) {
#ifdef _WIN32
    const int w = ::send(s, p, static_cast<int>(std::min<std::size_t>(n, 1 << 20)), 0);
#else
    const ssize_t w = ::send(s, p, n, 0);
#endif
    if (w <= 0) return false;
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}
bool sendStr(sock_t s, const std::string& t) { return sendAll(s, t.data(), t.size()); }

std::string htmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o.push_back(c);
    }
  }
  return o;
}
std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o.push_back(static_cast<char>(c));
    else { o.push_back('%'); o.push_back(hex[c >> 4]); o.push_back(hex[c & 15]); }
  }
  return o;
}

const char* statusText(int c) {
  switch (c) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 421: return "Misdirected Request";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

struct Ctx {
  fs::path root;  // canonical
  bool isolate = false;
  bool loopback = true;
  std::mutex* logMu = nullptr;
  std::ostream* log = nullptr;
  std::function<std::string()> stamp;
};

std::string headers(int code, const std::string& type, std::uint64_t len, const Ctx& c, const std::string& extra = "") {
  std::ostringstream h;
  h << "HTTP/1.1 " << code << " " << statusText(code) << "\r\nContent-Type: " << type << "\r\nContent-Length: " << len
    << "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nAccept-Ranges: bytes\r\nConnection: close\r\n";
  if (c.isolate) h << "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\nCross-Origin-Resource-Policy: same-origin\r\n";
  h << extra << "\r\n";
  return h.str();
}

void sendError(sock_t s, int code, const Ctx& c, bool head, const std::string& detail = "") {
  const std::string body = std::string(statusText(code)) + (detail.empty() ? "" : ": " + detail) + "\n";
  std::string extra = code == 405 ? "Allow: GET, HEAD\r\n" : "";
  sendStr(s, headers(code, "text/plain; charset=utf-8", body.size(), c, extra));
  if (!head) sendStr(s, body);
}

const char* kRunnerPage = R"HTML(<!doctype html>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WasmForge runner</title>
<style>body{margin:0;background:#111;color:#ddd;font:13px monospace}canvas{display:block;margin:0 auto;max-width:100%;background:#000}pre{margin:0;padding:8px;white-space:pre-wrap;max-height:35vh;overflow:auto}</style>
<canvas id="canvas" tabindex="-1" oncontextmenu="event.preventDefault()"></canvas><pre id="log">loading...</pre>
<script>
(async function () {
  var log = document.getElementById('log');
  function add(t) { log.textContent += t + '\n'; }
  try {
    var m = await (await fetch('manifest.json')).json();
    log.textContent = '';
    window.Module = { canvas: document.getElementById('canvas'), print: add, printErr: add };
    var s = document.createElement('script');
    s.src = m.entry;
    s.onerror = function () { add('could not load ' + m.entry); };
    document.body.appendChild(s);
  } catch (e) { log.textContent = 'wasmfrg: cannot start: ' + e; }
})();
</script>
)HTML";

const char* kReloadSnippet = "<script>(function(){var last=null;function poll(){fetch('/__wasmfrg/stamp',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){if(last===null)last=t;else if(t!==last)location.reload();}).catch(function(){}).then(function(){setTimeout(poll,1000);});}poll();})();</script>\n";

bool safeEntryName(const std::string& n) {
  if (n.empty() || n.size() > 128) return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) return false;
  return true;
}

std::string manifestEntry(const fs::path& root) {
  auto m = readFile(root / "manifest.json", 1u << 20);
  if (!m) return {};
  const auto k = m->find("\"entry\"");
  if (k == std::string::npos) return {};
  const auto q1 = m->find('"', m->find(':', k) + 1);
  if (q1 == std::string::npos) return {};
  const auto q2 = m->find('"', q1 + 1);
  if (q2 == std::string::npos) return {};
  const std::string e = m->substr(q1 + 1, q2 - q1 - 1);
  return safeEntryName(e) ? e : std::string();
}

struct Range { bool present = false, valid = true; std::uint64_t from = 0, to = 0; };
Range parseRange(const std::string& value, std::uint64_t size) {
  Range r;
  if (value.empty()) return r;
  r.present = true;
  if (value.compare(0, 6, "bytes=") != 0 || value.find(',') != std::string::npos) { r.valid = false; return r; }
  const std::string spec = value.substr(6);
  const auto dash = spec.find('-');
  if (dash == std::string::npos) { r.valid = false; return r; }
  const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
  auto num = [](const std::string& s, std::uint64_t& out) {
    if (s.empty() || s.size() > 15) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = std::stoull(s);
    return true;
  };
  std::uint64_t x = 0, y = 0;
  if (a.empty()) {  // suffix: last N bytes
    if (!num(b, y) || y == 0) { r.valid = false; return r; }
    r.from = y >= size ? 0 : size - y;
    r.to = size - 1;
  } else {
    if (!num(a, x)) { r.valid = false; return r; }
    r.from = x;
    r.to = b.empty() ? size - 1 : (num(b, y) ? std::min<std::uint64_t>(y, size - 1) : 0);
    if (!b.empty() && !num(b, y)) { r.valid = false; return r; }
  }
  if (size == 0 || r.from >= size || r.from > r.to) r.valid = false;
  return r;
}

void serveFile(sock_t s, const fs::path& file, const std::string& rangeHeader, bool head, const Ctx& c, int& statusOut, std::uint64_t& sentOut) {
  std::error_code ec;
  const std::uint64_t size = fs::file_size(file, ec);
  if (ec) { statusOut = 404; sendError(s, 404, c, head); return; }
  const Range rg = parseRange(rangeHeader, size);
  std::uint64_t from = 0, to = size ? size - 1 : 0;
  int code = 200;
  std::string extra;
  if (rg.present) {
    if (!rg.valid) {
      statusOut = 416;
      sendStr(s, headers(416, "text/plain; charset=utf-8", 0, c, "Content-Range: bytes */" + std::to_string(size) + "\r\n"));
      return;
    }
    from = rg.from; to = rg.to; code = 206;
    extra = "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size) + "\r\n";
  }
  const std::uint64_t len = size ? to - from + 1 : 0;
  statusOut = code;
  if (!sendStr(s, headers(code, mimeTypeFor(file.filename().string()), len, c, extra))) return;
  if (head || len == 0) return;
  std::ifstream f(file, std::ios::binary);
  if (!f) return;
  f.seekg(static_cast<std::streamoff>(from));
  std::vector<char> buf(64 * 1024);
  std::uint64_t left = len;
  while (left > 0 && f) {
    f.read(buf.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(buf.size(), left)));
    const auto n = static_cast<std::size_t>(f.gcount());
    if (n == 0) break;
    if (!sendAll(s, buf.data(), n)) break;
    left -= n;
    sentOut += n;
  }
}

std::string listing(const fs::path& dir, const std::string& urlPath) {
  std::ostringstream o;
  o << "<!doctype html><meta charset=utf-8><title>Index of " << htmlEscape(urlPath) << "</title><h3>Index of " << htmlEscape(urlPath) << "</h3><ul>";
  if (urlPath != "/") o << "<li><a href=\"../\">../</a>";
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) names.push_back(it->path().filename().u8string() + (it->is_directory(ec) ? "/" : ""));
  std::sort(names.begin(), names.end());
  for (const auto& n : names) o << "<li><a href=\"" << urlEncode(n) << "\">" << htmlEscape(n) << "</a>";
  o << "</ul>";
  return o.str();
}

std::string headerValue(const std::string& head, const std::string& name) {
  std::size_t pos = 0;
  while (pos < head.size()) {
    std::size_t e = head.find("\r\n", pos);
    if (e == std::string::npos) e = head.size();
    const std::string line = head.substr(pos, e - pos);
    const auto colon = line.find(':');
    if (colon != std::string::npos && lower(line.substr(0, colon)) == lower(name)) return trim(line.substr(colon + 1));
    pos = e + 2;
  }
  return {};
}

void handle(sock_t s, const Ctx& c) {
  // Read the request head (bounded).
  std::string head;
  char buf[2048];
  while (head.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
    const int n = ::recv(s, buf, sizeof buf, 0);
#else
    const ssize_t n = ::recv(s, buf, sizeof buf, 0);
#endif
    if (n <= 0) { closeSock(s); return; }
    head.append(buf, static_cast<std::size_t>(n));
    if (head.size() > kMaxHead) { sendError(s, 431, c, false); closeSock(s); return; }
  }
  const std::string reqLine = head.substr(0, head.find("\r\n"));
  const auto sp1 = reqLine.find(' '), sp2 = reqLine.rfind(' ');
  int status = 400;
  std::uint64_t sent = 0;
  std::string method, target;
  if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 > sp1) {
    method = reqLine.substr(0, sp1);
    target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
  }
  const bool isHead = method == "HEAD";
  do {
    if (method.empty() || !startsWith(reqLine.substr(sp2 + 1), "HTTP/1.")) { sendError(s, 400, c, false); break; }
    if (method != "GET" && !isHead) { status = 405; sendError(s, 405, c, false); break; }
    if (c.loopback) {  // DNS-rebinding guard: only accept requests addressed to localhost
      std::string host = lower(headerValue(head, "Host"));
      const auto colon = host.rfind(':');
      if (colon != std::string::npos && host.find(']') == std::string::npos) host = host.substr(0, colon);
      if (host != "localhost" && host != "127.0.0.1") { status = 421; sendError(s, 421, c, isHead, "use http://localhost or http://127.0.0.1"); break; }
    }
    std::string rel;
    if (!decodeRequestPath(target, rel)) { status = 400; sendError(s, 400, c, isHead, "invalid path"); break; }
    if (c.stamp && rel == "__wasmfrg/stamp") {
      const std::string body = c.stamp();
      status = 200;
      sendStr(s, headers(200, "text/plain; charset=utf-8", body.size(), c));
      if (!isHead) sendStr(s, body);
      break;
    }
    std::string urlPath = "/" + rel;
    std::error_code ec;
    fs::path p = rel.empty() ? c.root : fs::weakly_canonical(c.root / fromUtf8(rel), ec);
    if (ec || !isInside(c.root, p)) { status = 403; sendError(s, 403, c, isHead); break; }  // also catches symlinks pointing outside
    if (fs::is_directory(p, ec)) {
      if (!rel.empty() && target.find('?') == std::string::npos && !endsWith(target, "/")) {
        status = 302;
        sendStr(s, headers(302, "text/plain", 0, c, "Location: " + urlEncode(urlPath) + "/\r\n"));
        break;
      }
      if (fs::is_regular_file(p / "index.html", ec)) { p /= "index.html"; }
      else if (rel.empty()) {
        const std::string entry = manifestEntry(c.root);
        if (endsWith(entry, ".html")) { status = 302; sendStr(s, headers(302, "text/plain", 0, c, "Location: /" + entry + "\r\n")); break; }
        if (endsWith(entry, ".js")) {
          status = 200;
          const std::string page = std::string(kRunnerPage) + (c.stamp ? kReloadSnippet : "");
          sendStr(s, headers(200, "text/html; charset=utf-8", page.size(), c));
          if (!isHead) sendStr(s, page);
          break;
        }
        const std::string l = listing(p, "/");
        status = 200;
        sendStr(s, headers(200, "text/html; charset=utf-8", l.size(), c));
        if (!isHead) sendStr(s, l);
        break;
      } else {
        const std::string l = listing(p, urlPath);
        status = 200;
        sendStr(s, headers(200, "text/html; charset=utf-8", l.size(), c));
        if (!isHead) sendStr(s, l);
        break;
      }
    }
    if (!fs::is_regular_file(p, ec)) { status = 404; sendError(s, 404, c, isHead); break; }
    if (c.stamp && startsWith(mimeTypeFor(p.filename().string()), "text/html")) {
      if (auto page = readFile(p, 8u << 20)) {
        *page += kReloadSnippet;
        status = 200;
        sendStr(s, headers(200, "text/html; charset=utf-8", page->size(), c));
        if (!isHead) sendStr(s, *page);
        break;
      }
    }
    serveFile(s, p, headerValue(head, "Range"), isHead, c, status, sent);
  } while (false);
  {
    std::lock_guard<std::mutex> lk(*c.logMu);
    *c.log << "  " << method << " " << target << " -> " << status << (sent ? " (" + humanBytes(sent) + ")" : "") << std::endl;
  }
  closeSock(s);
}
}  // namespace

void requestServerStop() { g_stop = true; }
bool serverStopRequested() { return g_stop.load(); }

bool decodeRequestPath(const std::string& target, std::string& relOut) {
  relOut.clear();
  if (target.empty() || target[0] != '/') return false;
  std::string path = target.substr(0, target.find_first_of("?#"));
  std::string decoded;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%') {
      if (i + 2 >= path.size()) return false;  // truncated %XX escape
      auto hv = [](char ch) -> int { return ch >= '0' && ch <= '9' ? ch - '0' : (ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1)); };
      const int h = hv(path[i + 1]), l = hv(path[i + 2]);
      if (h < 0 || l < 0) return false;
      decoded.push_back(static_cast<char>(h * 16 + l));
      i += 2;
    } else {
      decoded.push_back(path[i]);
    }
  }
  if (decoded.find('\0') != std::string::npos || decoded.find('\\') != std::string::npos) return false;
  std::string out;
  for (const auto& seg : split(decoded, '/')) {
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") return false;  // never resolve upwards; reject instead of normalising
    for (char ch : seg)
      if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f || ch == ':') return false;  // control chars and drive-letter tricks
    out += (out.empty() ? "" : "/") + seg;
  }
  relOut = out;
  return true;
}

std::string mimeTypeFor(const std::string& name) {
  static const std::pair<const char*, const char*> t[] = {
      {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
      {".mjs", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"}, {".json", "application/json"},
      {".wasm", "application/wasm"}, {".txt", "text/plain; charset=utf-8"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"},
      {".wav", "audio/wav"}, {".ogg", "audio/ogg"}, {".mp3", "audio/mpeg"}, {".mid", "audio/midi"}, {".midi", "audio/midi"},
      {".wad", "application/octet-stream"}, {".data", "application/octet-stream"}};
  const std::string ext = lower(fs::path(name).extension().string());
  for (const auto& e : t)
    if (ext == e.first) return e.second;
  return "application/octet-stream";
}

int runServer(const ServeOptions& o, std::ostream& log) {
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(o.root, ec);
  if (ec || !fs::is_directory(root, ec)) { log << "error: not a directory: " << pathStr(o.root) << "\n"; return 1; }
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { log << "error: WSAStartup failed\n"; return 1; }
#else
  std::signal(SIGPIPE, SIG_IGN);
#endif
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(o.port));
  if (inet_pton(AF_INET, o.bind.c_str(), &addr.sin_addr) != 1) { log << "error: --bind must be an IPv4 address (got '" << o.bind << "')\n"; return 1; }
  const sock_t ls = ::socket(AF_INET, SOCK_STREAM, 0);
  if (ls == kBadSock) { log << "error: cannot create socket\n"; return 1; }
  int yes = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
  if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(ls, 16) != 0) {
    log << "error: cannot listen on " << o.bind << ":" << o.port << " (is the port already in use? try --port 0)\n";
    closeSock(ls);
    return 1;
  }
  socklen_t alen = sizeof addr;
  getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &alen);
  const int port = ntohs(addr.sin_port);

  std::mutex logMu;
  Ctx ctx;
  ctx.root = root;
  ctx.isolate = o.isolate;
  ctx.loopback = o.bind == "127.0.0.1";
  ctx.logMu = &logMu;
  ctx.log = &log;
  ctx.stamp = o.stamp;
  g_stop = false;
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  const std::string host = o.bind == "0.0.0.0" ? "localhost" : o.bind;
  log << "Serving " << pathStr(root) << "\n  open http://" << host << ":" << port << "/   (Ctrl-C to stop)" << std::endl;
  if (!ctx.loopback) log << "  warning: listening on " << o.bind << " - anyone on your network can read this directory\n";
  if (o.isolate) log << "  cross-origin isolation headers enabled (COOP/COEP)\n";
  if (o.stamp) log << "  live reload enabled\n";
  if (manifestEntry(root).empty() && !fs::exists(root / "index.html", ec))
    log << "  note: no manifest.json or index.html here; for a browser page build with --html or run `wasmfrg build` first\n";

  std::atomic<int> active{0};
  while (!g_stop) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(ls, &set);
    timeval tv{0, 500000};
    if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
#else
    pollfd pfd;
    pfd.fd = ls; pfd.events = POLLIN; pfd.revents = 0;
    if (::poll(&pfd, 1, 500) <= 0) continue;
#endif
    const sock_t cs = ::accept(ls, nullptr, nullptr);
    if (cs == kBadSock) continue;
#ifdef _WIN32
    DWORD tmo = 5000;
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
#else
    timeval tmo{5, 0};
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
#endif
    if (active.load() >= kMaxConnections) { sendError(cs, 503, ctx, false); closeSock(cs); continue; }
    active.fetch_add(1);
    std::thread([cs, &ctx, &active]() {
      handle(cs, ctx);
      active.fetch_sub(1);
    }).detach();
  }
  closeSock(ls);
  for (int i = 0; i < 40 && active.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  log << "stopped" << std::endl;
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
}  // namespace wf
