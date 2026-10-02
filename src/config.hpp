#pragma once

#include "color/controls.hpp"

#include <map>
#include <stdexcept>
#include <string>

namespace cc
{

class ConfigError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct Config
{
    int channels = 2;
    std::string scanPath = "/Volumes/mxl";
    std::string outputDomainDir;
    std::string outputDomainId;
    ClipMode clip = ClipMode::Legal;
    bool rgbClip = false;
    int wholeGrainOffset = 1;
    std::string nmosRegistryAddress;
    int nmosRegistryPort = 3210;
    bool nmosDnsSd = false;
    int nmosPort = 3292;
    std::string nmosSeed;
    int webPort = 8140;
    std::string hostId;
    std::string stateDir = "/config";
    std::string logLevel = "info";
    std::string configFile;
    int previewFps = 4;
};

[[nodiscard]] std::string hostnameString();
[[nodiscard]] bool knownSetting(std::string const& key);
[[nodiscard]] Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file);
[[nodiscard]] Config loadConfigFromEnv(std::map<std::string, std::string> const& env);

} // namespace cc
