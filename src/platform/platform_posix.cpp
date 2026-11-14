#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "core/util.hpp"
#include "platform/platform.hpp"

namespace wf {
namespace {
constexpr std::size_t kMaxCapture = 4u << 20;

void writeAll(int fd, const void* p, std::size_t n) {
  const ssize_t r = ::write(fd, p, n);
  (void)r;
}

std::uint64_t kbField(const std::string& text, const std::string& key) {
  const auto pos = text.find(key);
  if (pos == std::string::npos) return 0;
  return std::strtoull(text.c_str() + pos + key.size(), nullptr, 10) * 1024ULL;
}

std::uint64_t readU64(const char* path) {
  auto s = readFile(path, 64);
  if (!s) return 0;
  const std::string t = trim(*s);
  if (t.empty() || t == "max") return 0;
  return std::strtoull(t.c_str(), nullptr, 10);
}
}  // namespace

ProcessResult runProcess(const std::vector<std::string>& argv, const fs::path& cwd, bool capture, double timeoutSec) {
  using Clock = std::chrono::steady_clock;
  ProcessResult r;
  if (argv.empty()) {
    r.error = "empty command";
    return r;
  }
  for (const auto& a : argv)
    if (a.find('\0') != std::string::npos) {
      r.error = "argument contains NUL";
      return r;
    }
  int outp[2] = {-1, -1}, errp[2] = {-1, -1};
  if (capture && ::pipe(outp) != 0) {
    r.error = "pipe() failed";
    return r;
  }
  if (::pipe(errp) != 0) {
    r.error = "pipe() failed";
    return r;
  }
  ::fcntl(errp[1], F_SETFD, FD_CLOEXEC);  // closes on successful exec => parent sees EOF
  const auto t0 = Clock::now();
  const pid_t pid = ::fork();
  if (pid < 0) {
    r.error = "fork() failed";
    return r;
  }
  if (pid == 0) {
    if (timeoutSec > 0) ::setpgid(0, 0);  // own process group so a timeout can kill the whole tree
    if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
      int e = errno;
      writeAll(errp[1], &e, sizeof e);
      _exit(127);
    }
    if (capture) {
      ::dup2(outp[1], 1);
      ::dup2(outp[1], 2);
      ::close(outp[0]);
      ::close(outp[1]);
    }
    ::close(errp[0]);
    std::vector<char*> av;
    for (const auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    ::execvp(av[0], av.data());
    int e = errno;
    writeAll(errp[1], &e, sizeof e);
    _exit(127);
  }
  if (timeoutSec > 0) ::setpgid(pid, pid);  // also from the parent: avoids the fork/setpgid race
  ::close(errp[1]);
  const auto deadline = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeoutSec));
  auto killTree = [&]() {
    r.timedOut = true;
    if (::kill(-pid, SIGKILL) != 0) ::kill(pid, SIGKILL);
  };
  auto msLeft = [&]() -> int {  // -1 = wait forever
    if (timeoutSec <= 0) return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 500));
  };
  auto expired = [&]() { return timeoutSec > 0 && Clock::now() >= deadline; };

  if (capture) {
    ::close(outp[1]);
    struct pollfd pfd;
    pfd.fd = outp[0];
    pfd.events = POLLIN;
    for (;;) {
      if (expired()) { killTree(); break; }
      pfd.revents = 0;
      const int pr = ::poll(&pfd, 1, msLeft());
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pr == 0) continue;
      char buf[8192];
      const ssize_t n = ::read(outp[0], buf, sizeof buf);
      if (n <= 0) break;  // EOF: the child (and anything holding the pipe) is done
      if (r.output.size() < kMaxCapture) r.output.append(buf, static_cast<std::size_t>(n));
    }
    ::close(outp[0]);
  } else if (timeoutSec > 0) {
    for (;;) {
      siginfo_t info;
      std::memset(&info, 0, sizeof info);
      if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT | WNOHANG) == 0 && info.si_pid == pid) break;
      if (expired()) { killTree(); break; }
      ::usleep(20000);
    }
  }
  int childErr = 0;
  const ssize_t got = ::read(errp[0], &childErr, sizeof childErr);
  ::close(errp[0]);
  int status = 0;
  struct rusage ru;
  std::memset(&ru, 0, sizeof ru);
  ::wait4(pid, &status, 0, &ru);
  r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
#if defined(__APPLE__)
  r.peakRssKb = static_cast<std::uint64_t>(ru.ru_maxrss) / 1024;
#else
  r.peakRssKb = static_cast<std::uint64_t>(ru.ru_maxrss);
#endif
  if (got == static_cast<ssize_t>(sizeof childErr)) {
    r.started = false;
    r.error = "cannot execute '" + argv[0] + "': " + std::strerror(childErr);
    return r;
  }
  r.started = true;
  r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
  if (r.timedOut) r.error = "timed out after " + std::to_string(static_cast<int>(timeoutSec)) + " s and was killed";
  else if (WIFSIGNALED(status))
    r.error = std::string("terminated by signal ") + std::to_string(WTERMSIG(status)) +
              (WTERMSIG(status) == 9 ? " (SIGKILL: out-of-memory killer / Android phantom-process killer?)" : "");
  return r;
}

std::optional<fs::path> findInPath(const std::string& program) {
  if (program.find('/') != std::string::npos) {
    if (::access(program.c_str(), X_OK) == 0) return fs::path(program);
    return std::nullopt;
  }
  for (const auto& dir : split(envVar("PATH"), ':')) {
    if (dir.empty()) continue;
    fs::path p = fs::path(dir) / program;
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0) return p;
  }
  return std::nullopt;
}

MemoryInfo queryMemory() {
  MemoryInfo m;
  if (auto mi = readFile("/proc/meminfo", 1u << 20)) {
    m.totalBytes = kbField(*mi, "MemTotal:");
    m.availableBytes = kbField(*mi, "MemAvailable:");
    if (m.availableBytes == 0) m.availableBytes = kbField(*mi, "MemFree:") + kbField(*mi, "Cached:");
  }
  if (m.totalBytes == 0) {
    const long pages = ::sysconf(_SC_PHYS_PAGES), ps = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && ps > 0) m.totalBytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(ps);
  }
  // Container / cgroup limits (Docker, proot hosts, WSL2 with .wslconfig, ...).
  std::uint64_t limit = readU64("/sys/fs/cgroup/memory.max");
  std::uint64_t usage = readU64("/sys/fs/cgroup/memory.current");
  if (limit == 0) {
    limit = readU64("/sys/fs/cgroup/memory/memory.limit_in_bytes");
    usage = readU64("/sys/fs/cgroup/memory/memory.usage_in_bytes");
  }
  if (limit > 0 && m.totalBytes > 0 && limit < m.totalBytes) {
    m.totalBytes = limit;
    if (usage > 0) {
      const std::uint64_t free = limit > usage ? limit - usage : 0;
      if (m.availableBytes == 0 || free < m.availableBytes) m.availableBytes = free;
    }
  }
  m.ok = m.totalBytes > 0;
  return m;
}

unsigned queryCoreCount() {
  long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  unsigned cores = n > 0 ? static_cast<unsigned>(n) : 0;
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof set, &set) == 0) {
    const int c = CPU_COUNT(&set);
    if (c > 0 && (cores == 0 || static_cast<unsigned>(c) < cores)) cores = static_cast<unsigned>(c);
  }
#endif
  return cores;
}

std::uint64_t queryFreeDisk(const fs::path& where) {
  std::error_code ec;
  const auto si = fs::space(where, ec);
  return ec ? 0 : static_cast<std::uint64_t>(si.available);
}

std::string queryCpuModel() {
  if (auto ci = readFile("/proc/cpuinfo", 1u << 22)) {
    for (const char* key : {"model name", "Hardware", "Processor"}) {
      for (const auto& line : split(*ci, '\n')) {
        if (startsWith(line, key)) {
          const auto c = line.find(':');
          if (c != std::string::npos) {
            const std::string v = trim(line.substr(c + 1));
            if (!v.empty()) return v;
          }
        }
      }
    }
  }
  return "UNKNOWN";
}

std::string queryArch() {
  struct utsname u;
  if (::uname(&u) != 0) return "UNKNOWN";
  const std::string m = u.machine;
  if (m == "aarch64" || m == "arm64") return "arm64";
  if (m == "x86_64" || m == "amd64") return "x86_64";
  if (m.empty()) return "UNKNOWN";
  return m;
}

std::string osName() {
#if defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

std::string envVar(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

fs::path userConfigDir() {
  const std::string xdg = envVar("XDG_CONFIG_HOME");
  if (!xdg.empty()) return fs::path(xdg) / "wasmforge";
  const std::string home = envVar("HOME");
  if (!home.empty()) return fs::path(home) / ".config" / "wasmforge";
  return fs::temp_directory_path() / "wasmforge-config";
}

}  // namespace wf
#endif  // !_WIN32
