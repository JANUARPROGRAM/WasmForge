#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/util.hpp"
#include "hardware/hardware.hpp"
#include "project/project.hpp"

namespace wf {

enum class Profile { Auto, LowMemory, Balanced, Performance, Custom };
enum class Strategy { Intelligent, Compact, Execution, Memory, Balanced, None };
enum class AssetMode { Auto, Preserve, Embed, External, Transform };
enum class OutputKind { Web, WasmOnly, Runtime, Custom };

const char* toString(Profile);
const char* toString(Strategy);
const char* toString(AssetMode);
const char* toString(OutputKind);
bool parseEnum(const std::string&, Profile&);
bool parseEnum(const std::string&, Strategy&);
bool parseEnum(const std::string&, AssetMode&);
bool parseEnum(const std::string&, OutputKind&);

// ---- storage -------------------------------------------------------------------------------
using ConfigFile = std::map<std::string, std::string>;  // one layer: only explicitly set keys

struct ConfigValue {
  std::string value;
  std::string source;  // global | local | cli
};
using ConfigMap = std::map<std::string, ConfigValue>;

std::vector<std::string> configKeys();
std::string configKeyHelp(const std::string& key);
bool validateKeyValue(const std::string& key, const std::string& value, std::string& err);
bool loadConfigFile(const fs::path& path, ConfigFile& out, std::vector<std::string>& issues);
bool saveConfigFile(const fs::path& path, const ConfigFile& cfg);
std::string exportConfig(const ConfigFile& cfg);
fs::path globalConfigPath();
// Layering: AUTO defaults < global < local (project wasmforge.ini) < command line.
ConfigMap mergeLayers(const ConfigFile& global, const ConfigFile& local, const ConfigFile& cli);

// ---- resolution ----------------------------------------------------------------------------
struct Decision {
  std::string key, value, source, reason;  // source: auto | global | local | cli
};

struct ResolvedConfig {
  Profile requestedProfile = Profile::Auto;
  Profile profile = Profile::Balanced;  // effective profile after AUTO resolution
  Strategy strategy = Strategy::Balanced;
  AssetMode assets = AssetMode::Auto;
  OutputKind output = OutputKind::Web;
  std::string backend = "auto";
  std::string name = "app";
  bool html = false;
  unsigned workers = 1;
  std::uint64_t memoryMb = 512;
  unsigned perJobMb = 350;
  std::uint64_t cacheMb = 512;
  std::vector<Decision> decisions;
  std::vector<std::string> warnings;
};

// Every automatic decision is recorded in `decisions` with a human-readable reason.
ResolvedConfig resolveConfig(const ConfigMap& cm, const HardwareInfo& hw, const ProjectInfo& proj);
std::string describeDecisions(const ResolvedConfig& rc);
}  // namespace wf
