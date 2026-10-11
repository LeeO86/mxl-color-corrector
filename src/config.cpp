#include "config.hpp"

#include "util/json.hpp"
#include "util/uuid.hpp"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>

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

bool provided(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file, char const* key)
{
    return env.find(key) != env.end() || file.find(key) != file.end();
}

std::uint64_t parseU64(std::string const& text, bool& ok)
{
    ok = false;
    if (text.empty()) return 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return 0;
    ok = true;
    return static_cast<std::uint64_t>(v);
}
} // namespace

bool isAnnouncedAddress(std::string const& text)
{
    if (text.empty() || text == "0.0.0.0" || text == "::" || text == "::1") return false;
    in_addr ipv4{};
    if (inet_pton(AF_INET, text.c_str(), &ipv4) == 1)
    {
        auto const oct = ntohl(ipv4.s_addr);
        if ((oct >> 24) == 127) return false;
        return true;
    }
    in6_addr ipv6{};
    if (text.find(':') != std::string::npos && inet_pton(AF_INET6, text.c_str(), &ipv6) == 1)
    {
        return true;
    }
    return false;
}

std::string firstNonLoopbackIpv4()
{
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    std::string found;
    for (auto* it = list; it != nullptr; it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN] = {};
        auto const* addr = reinterpret_cast<sockaddr_in const*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) == nullptr) continue;
        if (isAnnouncedAddress(buf))
        {
            found = buf;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

std::string selectHostAddress(std::string const& configured, std::string const& hostIdAlias, std::string const& detected)
{
    if (!configured.empty())
    {
        if (!isAnnouncedAddress(configured))
        {
            throw ConfigError("NMOS_HOST_ADDRESS must be a non-loopback IP address, not a hostname");
        }
        return configured;
    }
    if (isAnnouncedAddress(hostIdAlias)) return hostIdAlias;
    if (isAnnouncedAddress(detected)) return detected;
    throw ConfigError("NMOS_HOST_ADDRESS is unset and no non-loopback IPv4 address was found");
}

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
    for (auto const& [k, _] : settingValues(Config{}))
    {
        if (key == k) return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> settingValues(Config const& cfg)
{
    auto const flag = [](bool v) { return std::string(v ? "true" : "false"); };
    Json tags = Json::object();
    for (auto const& [name, values] : cfg.nmosTags)
    {
        Json list = Json::array();
        for (auto const& value : values) list.push(Json::str(value));
        tags[name] = std::move(list);
    }
    return {{"CC_CHANNELS", std::to_string(cfg.channels)}, {"CC_CLIP", clipModeName(cfg.clip)}, {"CC_RGB_CLIP", cfg.rgbClip ? "on" : "off"},
        {"CC_READ_OFFSET_GRAINS", std::to_string(cfg.wholeGrainOffset)}, {"CC_PREVIEW_FPS", std::to_string(cfg.previewFps)},
        {"CC_LOG_LEVEL", cfg.logLevel}, {"CC_CONFIG_FILE", cfg.configFile}, {"CC_STATE_DIR", cfg.stateDir}, {"MXL_DOMAIN_SCAN_PATH", cfg.scanPath},
        {"MXL_OUTPUT_DOMAIN_DIR", cfg.outputDomainDir}, {"MXL_OUTPUT_DOMAIN_ID", cfg.outputDomainId},
        {"MXL_HISTORY_DURATION_NS", std::to_string(cfg.historyDurationNs)}, {"MXL_CLEANUP_ON_EXIT", flag(cfg.cleanupOnExit)},
        {"NMOS_REGISTRY_ADDRESS", cfg.nmosRegistryAddress}, {"NMOS_REGISTRY_PORT", std::to_string(cfg.nmosRegistryPort)},
        {"NMOS_QUERY_ADDRESS", cfg.nmosQueryAddress}, {"NMOS_QUERY_PORT", std::to_string(cfg.nmosQueryPort)}, {"NMOS_DNS_SD", flag(cfg.nmosDnsSd)},
        {"NMOS_PORT", std::to_string(cfg.nmosPort)}, {"NMOS_SEED", cfg.nmosSeed}, {"NMOS_LABEL", cfg.nmosLabel}, {"NMOS_TAGS", tags.dump()},
        {"NMOS_HOST_ADDRESS", cfg.nmosHostAddress}, {"HOST_ID", cfg.hostId}, {"WEB_PORT", std::to_string(cfg.webPort)},
        {"WIDGET_FRAME_ANCESTORS", cfg.widgetFrameAncestors}, {"SHUTDOWN_TIMEOUT_S", std::to_string(cfg.shutdownTimeoutS)}};
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
    bool const hostIdSet = provided(env, file, "HOST_ID");
    cfg.hostId = lookup(env, file, "HOST_ID", hostnameString());
    cfg.configFile = lookup(env, file, "CC_CONFIG_FILE", "");
    auto const label = lookup(env, file, "NMOS_LABEL", "");
    cfg.nmosLabel = !label.empty() ? label : (hostIdSet ? cfg.hostId : "MXL Color Corrector");
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
    cfg.nmosQueryAddress = lookup(env, file, "NMOS_QUERY_ADDRESS", cfg.nmosRegistryAddress);
    if (provided(env, file, "NMOS_QUERY_PORT"))
    {
        cfg.nmosQueryPort = parseInt(lookup(env, file, "NMOS_QUERY_PORT", ""), ok);
        if (!ok || cfg.nmosQueryPort < 1 || cfg.nmosQueryPort > 65535)
        {
            throw ConfigError("NMOS_QUERY_PORT is invalid");
        }
    }
    else if (cfg.nmosRegistryPort < 65535)
    {
        cfg.nmosQueryPort = cfg.nmosRegistryPort + 1;
    }
    else
    {
        throw ConfigError("NMOS_QUERY_PORT must be set when NMOS_REGISTRY_PORT is 65535");
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
    cfg.shutdownTimeoutS = parseInt(lookup(env, file, "SHUTDOWN_TIMEOUT_S", "10"), ok);
    if (!ok || cfg.shutdownTimeoutS < 1 || cfg.shutdownTimeoutS > 120)
    {
        throw ConfigError("SHUTDOWN_TIMEOUT_S must be from 1 to 120");
    }
    cfg.cleanupOnExit = parseBool(lookup(env, file, "MXL_CLEANUP_ON_EXIT", "false"), ok);
    if (!ok)
    {
        throw ConfigError("MXL_CLEANUP_ON_EXIT must be true or false");
    }
    cfg.historyDurationNs = parseU64(lookup(env, file, "MXL_HISTORY_DURATION_NS", "200000000"), ok);
    if (!ok || cfg.historyDurationNs == 0)
    {
        throw ConfigError("MXL_HISTORY_DURATION_NS must be a positive integer");
    }
    auto const tags = lookup(env, file, "NMOS_TAGS", "");
    if (!tags.empty())
    {
        Json doc;
        try
        {
            doc = parseJson(tags);
        }
        catch (JsonError const& ex)
        {
            throw ConfigError(std::string("NMOS_TAGS is not JSON: ") + ex.what());
        }
        if (!doc.isObject()) throw ConfigError("NMOS_TAGS must be a JSON object of string arrays");
        for (auto const& [key, value] : doc.o)
        {
            if (!value.isArray()) throw ConfigError("NMOS_TAGS values must be arrays of strings");
            std::vector<std::string> items;
            for (auto const& item : value.a)
            {
                if (!item.isString()) throw ConfigError("NMOS_TAGS values must be arrays of strings");
                items.push_back(item.s);
            }
            cfg.nmosTags.emplace_back(key, std::move(items));
        }
    }
    cfg.widgetFrameAncestors = lookup(env, file, "WIDGET_FRAME_ANCESTORS", "");
    if (cfg.widgetFrameAncestors.find_first_not_of(' ') == std::string::npos)
    {
        cfg.widgetFrameAncestors = Config{}.widgetFrameAncestors;
    }
    // One CSP directive's source list: a ';' or ',' would start another directive or policy.
    for (unsigned char const c : cfg.widgetFrameAncestors)
    {
        if (c < 0x20 || c == 0x7f || c == ';' || c == ',')
        {
            throw ConfigError("WIDGET_FRAME_ANCESTORS must be a CSP source list, e.g. 'self' https://designer.example");
        }
    }
    try
    {
        cfg.nmosHostAddress = selectHostAddress(lookup(env, file, "NMOS_HOST_ADDRESS", ""), hostIdSet ? cfg.hostId : "", firstNonLoopbackIpv4());
    }
    catch (ConfigError const&)
    {
        throw;
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
    for (auto const& [key, _] : settingValues(cfg))
    {
        cfg.origins[key] = env.count(key) ? "environment" : file.count(key) ? "file" : "default";
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
