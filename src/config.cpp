#include "config.hpp"

#include "util/json.hpp"
#include "util/uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace cc
{
namespace
{
bool parseBool(std::string const& text, bool& ok)
{
    ok = true;
    if (text == "1" || text == "true" || text == "on" || text == "yes") return true;
    if (text == "0" || text == "false" || text == "off" || text == "no") return false;
    ok = false;
    return false;
}

int parseInt(std::string const& text, bool& ok)
{
    ok = false;
    if (text.empty()) return 0;
    char* end = nullptr;
    long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return 0;
    ok = true;
    return static_cast<int>(v);
}

std::string lookup(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file, char const* key, std::string fallback)
{
    if (auto it = env.find(key); it != env.end()) return it->second;
    if (auto it = file.find(key); it != file.end()) return it->second;
    return fallback;
}
} // namespace

std::string hostnameString()
{
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0 || buf[0] == '\0')
    {
        return "localhost";
    }
    return buf;
}

bool knownSetting(std::string const& key)
{
    static char const* keys[] = {"CC_CHANNELS", "MXL_DOMAIN_SCAN_PATH", "MXL_OUTPUT_DOMAIN_DIR", "MXL_OUTPUT_DOMAIN_ID", "CC_CLIP", "CC_RGB_CLIP",
        "CC_READ_OFFSET_GRAINS", "NMOS_REGISTRY_ADDRESS", "NMOS_REGISTRY_PORT", "NMOS_DNS_SD", "NMOS_PORT", "NMOS_SEED", "WEB_PORT", "CC_CONFIG_FILE",
        "HOST_ID", "CC_STATE_DIR", "CC_LOG_LEVEL", "CC_PREVIEW_FPS"};
    for (auto const* k : keys)
    {
        if (key == k) return true;
    }
    return false;
}

Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file)
{
    for (auto const& [key, _] : file)
    {
        if (!knownSetting(key))
        {
            throw ConfigError("unknown configuration key " + key);
        }
    }
    Config cfg;
    cfg.hostId = lookup(env, file, "HOST_ID", hostnameString());
    cfg.configFile = lookup(env, file, "CC_CONFIG_FILE", "");
    bool ok = true;
    cfg.channels = parseInt(lookup(env, file, "CC_CHANNELS", "2"), ok);
    if (!ok || cfg.channels < 1 || cfg.channels > 16)
    {
        throw ConfigError("CC_CHANNELS must be an integer from 1 to 16");
    }
    cfg.scanPath = lookup(env, file, "MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl");
    cfg.outputDomainDir = lookup(env, file, "MXL_OUTPUT_DOMAIN_DIR", "");
    cfg.outputDomainId = lookup(env, file, "MXL_OUTPUT_DOMAIN_ID", "");
    auto clipText = lookup(env, file, "CC_CLIP", "legal");
    cfg.clip = parseClipMode(clipText, ok);
    if (!ok)
    {
        throw ConfigError("CC_CLIP must be legal, extended or off");
    }
    auto rgb = lookup(env, file, "CC_RGB_CLIP", "off");
    cfg.rgbClip = parseBool(rgb, ok);
    if (!ok)
    {
        throw ConfigError("CC_RGB_CLIP must be on or off");
    }
    auto offset = lookup(env, file, "CC_READ_OFFSET_GRAINS", "");
    if (!offset.empty())
    {
        cfg.wholeGrainOffset = parseInt(offset, ok);
        if (!ok || cfg.wholeGrainOffset < 0 || cfg.wholeGrainOffset > 8)
        {
            throw ConfigError("CC_READ_OFFSET_GRAINS must be an integer from 0 to 8");
        }
    }
    cfg.nmosRegistryAddress = lookup(env, file, "NMOS_REGISTRY_ADDRESS", "");
    cfg.nmosRegistryPort = parseInt(lookup(env, file, "NMOS_REGISTRY_PORT", "3210"), ok);
    if (!ok || cfg.nmosRegistryPort < 1 || cfg.nmosRegistryPort > 65535)
    {
        throw ConfigError("NMOS_REGISTRY_PORT is invalid");
    }
    cfg.nmosDnsSd = parseBool(lookup(env, file, "NMOS_DNS_SD", "false"), ok);
    if (!ok)
    {
        throw ConfigError("NMOS_DNS_SD must be true or false");
    }
    cfg.nmosPort = parseInt(lookup(env, file, "NMOS_PORT", "3292"), ok);
    if (!ok || cfg.nmosPort < 0 || cfg.nmosPort > 65535)
    {
        throw ConfigError("NMOS_PORT is invalid");
    }
    cfg.webPort = parseInt(lookup(env, file, "WEB_PORT", "8140"), ok);
    if (!ok || cfg.webPort < 0 || cfg.webPort > 65535)
    {
        throw ConfigError("WEB_PORT is invalid");
    }
    cfg.nmosSeed = lookup(env, file, "NMOS_SEED", cfg.hostId + "-cc");
    if (cfg.nmosSeed.empty())
    {
        throw ConfigError("NMOS_SEED is empty");
    }
    cfg.stateDir = lookup(env, file, "CC_STATE_DIR", "/config");
    cfg.logLevel = lookup(env, file, "CC_LOG_LEVEL", "info");
    cfg.previewFps = parseInt(lookup(env, file, "CC_PREVIEW_FPS", "4"), ok);
    if (!ok || cfg.previewFps < 1 || cfg.previewFps > 30)
    {
        throw ConfigError("CC_PREVIEW_FPS must be from 1 to 30");
    }
    if (cfg.outputDomainId.empty())
    {
        cfg.outputDomainId = uuidV5("mxl-color-corrector/" + cfg.nmosSeed + "/domain");
    }
    else if (!isUuid(cfg.outputDomainId))
    {
        throw ConfigError("MXL_OUTPUT_DOMAIN_ID must be a UUID");
    }
    if (cfg.outputDomainDir.empty())
    {
        auto compact = cfg.outputDomainId;
        compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
        cfg.outputDomainDir = cfg.scanPath + "/cc-" + compact.substr(0, 8);
    }
    return cfg;
}

Config loadConfigFromEnv(std::map<std::string, std::string> const& env)
{
    std::map<std::string, std::string> file;
    std::string path;
    if (auto it = env.find("CC_CONFIG_FILE"); it != env.end()) path = it->second;
    if (!path.empty())
    {
        std::ifstream in(path);
        if (!in)
        {
            throw ConfigError("cannot read CC_CONFIG_FILE " + path);
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        Json doc;
        try
        {
            doc = parseJson(buffer.str());
        }
        catch (JsonError const& ex)
        {
            throw ConfigError(std::string("invalid configuration json: ") + ex.what());
        }
        if (!doc.isObject())
        {
            throw ConfigError("configuration file must be a JSON object");
        }
        for (auto const& [key, value] : doc.o)
        {
            if (value.isString()) file[key] = value.s;
            else if (value.isNumber() || value.isBool()) file[key] = value.text();
            else throw ConfigError("configuration value for " + key + " must be a scalar");
        }
    }
    return loadConfig(env, file);
}

} // namespace cc
