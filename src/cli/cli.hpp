#pragma once
#include <string>
#include <vector>

namespace wf {
// args[0] is the program name. Exit codes: 0 ok | 1 failure | 2 usage | 3 environment | 4 configuration.
int runCli(const std::vector<std::string>& args);
}  // namespace wf
