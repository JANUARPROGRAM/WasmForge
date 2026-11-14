#if defined(_WIN32)
// Native Windows implementation (Win32 API, no cmd.exe unless a .bat/.cmd launcher is unavoidable).
#include <windows.h>

#include <chrono>
#include <thread>

#include "core/util.hpp"
#include "platform/platform.hpp"

namespace wf {
namespace {
constexpr std::size_t kMaxCapture = 4u << 20;

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

// Quoting rules of CommandLineToArgvW / the MSVC CRT.
std::wstring quoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  for (auto it = a.begin();; ++it) {
    unsigned bs = 0;
    while (it != a.end() && *it == L'\\') {
      ++it;
      ++bs;
    }
    if (it == a.end()) {
      out.append(bs * 2, L'\\');
      break;
    } else if (*it == L'"') {
      out.append(bs * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(bs, L'\\');
      out.push_back(*it);
    }
  }
  out.push_back(L'"');
  return out;
}

std::uint64_t memFree(const MEMORYSTATUSEX& ms) { return static_cast<std::uint64_t>(ms.ullAvailPhys); }
}  // namespace

std::optional<fs::path> findInPath(const std::string& program) {
  std::vector<std::string> exts = {""};
  if (fs::path(program).extension().empty()) {
    std::string pe = envVar("PATHEXT");
    if (pe.empty()) pe = ".COM;.EXE;.BAT;.CMD";
    for (const auto& e : split(pe, ';'))
      if (!e.empty()) exts.push_back(lower(e));
  }
  std::vector<fs::path> dirs;
  if (program.find_first_of("/\\") != std::string::npos) dirs.push_back(fs::path());
  else
    for (const auto& d : split(envVar("PATH"), ';'))
      if (!d.empty()) dirs.push_back(fromUtf8(d));
  for (const auto& d : dirs)
    for (const auto& e : exts) {
      fs::path p = d.empty() ? fromUtf8(program + e) : d / fromUtf8(program + e);
      std::error_code ec;
      if (fs::is_regular_file(p, ec)) return p;
    }
  return std::nullopt;
}

ProcessResult runProcess(const std::vector<std::string>& argv, const fs::path& cwd, bool capture, double timeoutSec) {
  ProcessResult r;
  if (argv.empty()) {
    r.error = "empty command";
    return r;
  }
  fs::path resolved = fromUtf8(argv[0]);
  if (auto f = findInPath(argv[0])) resolved = *f;
  const std::string ext = lower(resolved.extension().string());
  std::wstring cmdline;
  if (ext == ".bat" || ext == ".cmd") {
    // Batch launchers (emcc.bat, em++.bat) can only run through cmd.exe, which re-parses the line.
    // We therefore REFUSE any argument containing cmd.exe metacharacters instead of trying to escape them.
    std::vector<std::string> all = argv;
    all[0] = pathStr(resolved);
    for (const auto& a : all)
      if (a.find_first_of("\"%^&|<>!\r\n") != std::string::npos) {
        r.error = "argument contains characters unsafe for cmd.exe batch launchers: " + a;
        return r;
      }
    std::wstring inner;
    for (std::size_t i = 0; i < all.size(); ++i) {
      std::wstring w = widen(i == 0 ? resolved.u8string() : all[i]);
      if (i) inner += L' ';
      inner += (i == 0 || w.empty() || w.find_first_of(L" \t") != std::wstring::npos) ? L"\"" + w + L"\"" : w;
    }
    cmdline = L"cmd.exe /d /s /c \"" + inner + L"\"";
  } else {
    cmdline = quoteArg(widen(resolved.u8string()));
    for (std::size_t i = 1; i < argv.size(); ++i) cmdline += L" " + quoteArg(widen(argv[i]));
  }
  std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
  buf.push_back(L'\0');

  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  STARTUPINFOW si{};
  si.cb = sizeof si;
  if (capture) {
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
      r.error = "CreatePipe failed";
      return r;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  PROCESS_INFORMATION pi{};
  const auto t0 = std::chrono::steady_clock::now();
  const std::wstring wcwd = cwd.empty() ? L"" : cwd.wstring();
  const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, capture ? TRUE : FALSE, 0, nullptr,
                                 wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
  if (capture) CloseHandle(wr);
  if (!ok) {
    if (capture) CloseHandle(rd);
    r.error = "cannot execute '" + argv[0] + "' (Win32 error " + std::to_string(GetLastError()) + ")";
    return r;
  }
  // A reader thread drains the pipe so the main thread can wait with a timeout.
  std::thread reader;
  if (capture)
    reader = std::thread([&]() {
      char chunk[8192];
      DWORD n = 0;
      while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n > 0)
        if (r.output.size() < kMaxCapture) r.output.append(chunk, n);
    });
  const DWORD waitMs = timeoutSec > 0 ? static_cast<DWORD>(timeoutSec * 1000.0) : INFINITE;
  if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT) {
    r.timedOut = true;
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  if (capture) {
    reader.join();  // pipe closes when the process (and its children) exit
    CloseHandle(rd);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  r.started = true;
  r.exitCode = static_cast<int>(code);
  r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (r.timedOut) r.error = "timed out after " + std::to_string(static_cast<int>(timeoutSec)) + " s and was killed";
  return r;  // peakRssKb stays 0 (= unknown) on Windows
}

MemoryInfo queryMemory() {
  MemoryInfo m;
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof ms;
  if (GlobalMemoryStatusEx(&ms)) {
    m.ok = true;
    m.totalBytes = static_cast<std::uint64_t>(ms.ullTotalPhys);
    m.availableBytes = memFree(ms);
  }
  return m;
}

unsigned queryCoreCount() {
  const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return n > 0 ? static_cast<unsigned>(n) : 0;
}

std::uint64_t queryFreeDisk(const fs::path& where) {
  std::error_code ec;
  const auto si = fs::space(where, ec);
  return ec ? 0 : static_cast<std::uint64_t>(si.available);
}

std::string queryCpuModel() {
  char buf[256];
  DWORD sz = sizeof buf;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                   RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
    return trim(buf);
  return "UNKNOWN";
}

std::string queryArch() {
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default: return "UNKNOWN";
  }
}

std::string osName() { return "windows"; }

std::string envVar(const char* name) {
  const std::wstring wn = widen(name);
  DWORD n = GetEnvironmentVariableW(wn.c_str(), nullptr, 0);
  if (n == 0) return {};
  std::wstring w(n, L'\0');
  n = GetEnvironmentVariableW(wn.c_str(), &w[0], n);
  w.resize(n);
  if (w.empty()) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], len, nullptr, nullptr);
  return s;
}

fs::path userConfigDir() {
  const std::string a = envVar("APPDATA");
  if (!a.empty()) return fromUtf8(a) / "WasmForge";
  return fs::temp_directory_path() / "WasmForge-config";
}

}  // namespace wf
#endif  // _WIN32
