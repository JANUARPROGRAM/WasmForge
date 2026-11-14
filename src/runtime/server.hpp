#pragma once
#include <functional>
#include <ostream>
#include <string>

#include "core/util.hpp"

namespace wf {

struct ServeOptions {
  fs::path root;
  std::string bind = "127.0.0.1";  // loopback by default: nothing is exposed to the network unless asked
  int port = 8080;                 // 0 = pick a free port
  bool isolate = false;            // COOP/COEP headers (needed by SharedArrayBuffer / wasm threads)
  std::function<std::string()> stamp;  // when set: enables live reload (/__wasmfrg/stamp + a small script in served HTML)
};

// Tiny static HTTP/1.1 server for testing builds in a browser (also on a phone via localhost).
// GET/HEAD only, Range supported, correct application/wasm MIME type, path-traversal and symlink-escape
// protection, Host-header check on loopback (DNS-rebinding), bounded request size and connection count.
// Blocks until Ctrl-C. Returns 0 on clean shutdown, 1 on startup failure.
int runServer(const ServeOptions& o, std::ostream& log);

// Cooperative shutdown (the server also stops on SIGINT/SIGTERM).
void requestServerStop();
bool serverStopRequested();

// Exposed for unit tests.
bool decodeRequestPath(const std::string& target, std::string& relOut);  // false => reject (traversal, NUL, bad escape)
std::string mimeTypeFor(const std::string& fileName);
}  // namespace wf
