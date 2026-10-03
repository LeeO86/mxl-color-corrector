#pragma once

#include "color/controls.hpp"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    std::string nmosQueryAddress;
    int nmosQueryPort = 3211;
    bool nmosDnsSd = false;
    int nmosPort = 3292;
    std::string nmosSeed;
    std::string nmosLabel = "MXL Color Corrector";
    std::string nmosHostAddress;
    std::vector<std::pair<std::string, std::vector<std::string>>> nmosTags;
    int webPort = 8140;
    std::string hostId;
    std::string stateDir = "/config";
    std::string logLevel = "info";
    std::string configFile;
    int previewFps = 4;
    int shutdownTimeoutS = 10;
    bool cleanupOnExit = false;
    std::uint64_t historyDurationNs = 200000000;
};

// An address we may announce to other systems: an IP literal that is not
// 0.0.0.0, not loopback, and not empty.
[[nodiscard]] bool isAnnouncedAddress(std::string const& text);
[[nodiscard]] std::string firstNonLoopbackIpv4();
// NMOS_HOST_ADDRESS, else HOST_ID when that value is an IP literal, else detected.
[[nodiscard]] std::string selectHostAddress(std::string const& configured, std::string const& hostIdAlias, std::string const& detected);

[[nodiscard]] std::string hostnameString();
[[nodiscard]] bool knownSetting(std::string const& key);
[[nodiscard]] Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file);
[[nodiscard]] Config loadConfigFromEnv(std::map<std::string, std::string> const& env);

} // namespace cc
