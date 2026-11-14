#include <string>
#include <vector>

#include "cli/cli.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

// Windows: take the command line as UTF-16 and hand UTF-8 to the portable core.
int main() {
  SetConsoleOutputCP(CP_UTF8);
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  std::vector<std::string> args;
  for (int i = 0; i < n; ++i) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? static_cast<std::size_t>(len - 1) : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
    args.push_back(s);
  }
  LocalFree(w);
  return wf::runCli(args);
}
#else
int main(int argc, char** argv) { return wf::runCli(std::vector<std::string>(argv, argv + argc)); }
#endif
