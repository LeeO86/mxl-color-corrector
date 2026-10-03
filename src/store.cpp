#include "store.hpp"

#include "util/json.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace cc
{
namespace
{
bool readNumber(Json const* node, double& dest)
{
    if (node == nullptr) return true;
    if (!node->isNumber() && !node->isString()) return false;
    dest = node->num();
    return true;
}

bool readRgb(Json const* node, RgbTrim& dest)
{
    if (node == nullptr) return true;
    if (!node->isObject()) return false;
    return readNumber(node->find("r"), dest.r) && readNumber(node->find("g"), dest.g) && readNumber(node->find("b"), dest.b);
}

void putRgb(Json& obj, char const* key, RgbTrim const& rgb)
{
    Json v = Json::object();
    v["r"] = Json::number(rgb.r);
    v["g"] = Json::number(rgb.g);
    v["b"] = Json::number(rgb.b);
    obj[key] = std::move(v);
}

Json wheelJson(double x, double y)
{
    Json v = Json::object();
    v["x"] = Json::number(x);
    v["y"] = Json::number(y);
    return v;
}

void resetOne(Controls& controls, std::string const& name, Controls const& fresh)
{
    if (name == "white" || name == "all")
    {
        controls.white = {};
        controls.whiteWheelX = 0;
        controls.whiteWheelY = 0;
    }
    if (name == "black" || name == "all")
    {
        controls.black = {};
        controls.blackWheelX = 0;
        controls.blackWheelY = 0;
    }
    if (name == "gain" || name == "all") controls.gain = fresh.gain;
    if (name == "pedestal" || name == "all") controls.pedestal = fresh.pedestal;
    if (name == "brightness" || name == "all") controls.brightness = fresh.brightness;
    if (name == "saturation" || name == "all") controls.saturation = fresh.saturation;
    if (name == "bypass" || name == "all") controls.bypass = false;
}

Json presetToJson(Preset const& preset)
{
    Json obj = Json::object();
    obj["name"] = Json::str(preset.name);
    Json channels = Json::array();
    for (auto const& controls : preset.channels)
    {
        channels.push(controlsToJson(controls));
    }
    obj["channels"] = std::move(channels);
    return obj;
}

bool presetFromJson(Json const& obj, Preset& preset, int expect, std::string& error)
{
    if (!obj.isObject() || obj.find("name") == nullptr)
    {
        error = "preset needs a name";
        return false;
    }
    preset.name = obj.find("name")->text();
    if (preset.name.empty() || preset.name.size() > 64)
    {
        error = "preset name is invalid";
        return false;
    }
    auto const* channels = obj.find("channels");
    if (channels == nullptr || !channels->isArray())
    {
        error = "preset channels must be an array";
        return false;
    }
    preset.channels.clear();
    for (auto const& item : channels->a)
    {
        Controls controls;
        if (!patchControls(controls, item, error)) return false;
        preset.channels.push_back(controls);
    }
    if (expect > 0 && static_cast<int>(preset.channels.size()) != expect)
    {
        error = "preset channel count does not match";
        return false;
    }
    return true;
}
} // namespace

Json controlsToJson(Controls const& c)
{
    Json obj = Json::object();
    putRgb(obj, "white", c.white);
    putRgb(obj, "black", c.black);
    obj["white_wheel"] = wheelJson(c.whiteWheelX, c.whiteWheelY);
    obj["black_wheel"] = wheelJson(c.blackWheelX, c.blackWheelY);
    obj["gain"] = Json::number(c.gain);
    obj["pedestal"] = Json::number(c.pedestal);
    obj["brightness"] = Json::number(c.brightness);
    obj["saturation"] = Json::number(c.saturation);
    obj["bypass"] = Json::boolean(c.bypass);
    obj["clip"] = Json::str(clipModeName(c.clip));
    obj["rgb_clip"] = Json::boolean(c.rgbClip);
    return obj;
}

bool patchControls(Controls& controls, Json const& patch, std::string& error)
{
    if (!patch.isObject())
    {
        error = "controls must be an object";
        return false;
    }
    Controls next = controls;
    bool whiteNumeric = patch.find("white") != nullptr;
    bool blackNumeric = patch.find("black") != nullptr;
    if (!readRgb(patch.find("white"), next.white) || !readRgb(patch.find("black"), next.black))
    {
        error = "white and black must be objects with r, g, b";
        return false;
    }
    if (!readNumber(patch.find("gain"), next.gain) || !readNumber(patch.find("pedestal"), next.pedestal) ||
        !readNumber(patch.find("brightness"), next.brightness) || !readNumber(patch.find("saturation"), next.saturation))
    {
        error = "numeric control is not a number";
        return false;
    }
    bool whiteWheel = false;
    bool blackWheel = false;
    if (auto const* wheel = patch.find("white_wheel"))
    {
        if (!wheel->isObject() || !readNumber(wheel->find("x"), next.whiteWheelX) || !readNumber(wheel->find("y"), next.whiteWheelY))
        {
            error = "white_wheel must be {x,y}";
            return false;
        }
        whiteWheel = true;
    }
    if (auto const* wheel = patch.find("black_wheel"))
    {
        if (!wheel->isObject() || !readNumber(wheel->find("x"), next.blackWheelX) || !readNumber(wheel->find("y"), next.blackWheelY))
        {
            error = "black_wheel must be {x,y}";
            return false;
        }
        blackWheel = true;
    }
    if (auto const* bypass = patch.find("bypass"))
    {
        if (!bypass->isBool())
        {
            error = "bypass must be a boolean";
            return false;
        }
        next.bypass = bypass->b;
    }
    if (auto const* clip = patch.find("clip"))
    {
        bool ok = false;
        auto mode = parseClipMode(clip->text(), ok);
        if (!ok)
        {
            error = "clip must be legal, extended or off";
            return false;
        }
        next.clip = mode;
    }
    if (auto const* rgb = patch.find("rgb_clip"))
    {
        if (!rgb->isBool())
        {
            error = "rgb_clip must be a boolean";
            return false;
        }
        next.rgbClip = rgb->b;
    }
    if (whiteWheel)
    {
        auto trims = wheelToTrims(next.whiteWheelX, next.whiteWheelY, 20);
        next.white = {trims.r, trims.g, trims.b};
    }
    else if (whiteNumeric)
    {
        trimsToWheel(next.white.r, next.white.g, next.white.b, 20, next.whiteWheelX, next.whiteWheelY);
    }
    if (blackWheel)
    {
        auto trims = wheelToTrims(next.blackWheelX, next.blackWheelY, 5);
        next.black = {trims.r, trims.g, trims.b};
    }
    else if (blackNumeric)
    {
        trimsToWheel(next.black.r, next.black.g, next.black.b, 5, next.blackWheelX, next.blackWheelY);
    }
    error = validateControls(next);
    if (!error.empty()) return false;
    controls = next;
    return true;
}

ControlStore::ControlStore(Config config)
    : config_(std::move(config))
{
    channels_.resize(static_cast<std::size_t>(config_.channels));
    matrices_.resize(channels_.size());
    auto fresh = defaultControls();
    for (auto& slot : channels_)
    {
        slot.a = fresh;
        slot.b = fresh;
    }
    loadLocked();
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        matrices_[i] = buildMatrix(channels_[i].live(), generation_);
    }
}

Controls ControlStore::defaultControls() const
{
    Controls controls;
    controls.clip = config_.clip;
    controls.rgbClip = config_.rgbClip;
    return controls;
}

int ControlStore::channels() const
{
    std::lock_guard lock(mu_);
    return static_cast<int>(channels_.size());
}

bool ControlStore::validChannel(int channel) const
{
    return channel >= 1 && channel <= static_cast<int>(channels_.size());
}

Controls ControlStore::live(int channel) const
{
    std::lock_guard lock(mu_);
    if (!validChannel(channel)) return {};
    return channels_[static_cast<std::size_t>(channel - 1)].live();
}

ChannelSlots ControlStore::slots(int channel) const
{
    std::lock_guard lock(mu_);
    if (!validChannel(channel)) return {};
    return channels_[static_cast<std::size_t>(channel - 1)];
}

FixedMatrix ControlStore::matrix(int channel) const
{
    std::lock_guard lock(mu_);
    if (!validChannel(channel)) return {};
    return matrices_[static_cast<std::size_t>(channel - 1)];
}

char ControlStore::activeSlot(int channel) const
{
    std::lock_guard lock(mu_);
    if (!validChannel(channel)) return 'a';
    return channels_[static_cast<std::size_t>(channel - 1)].active;
}

void ControlStore::publishLocked(int channel)
{
    auto& slot = channels_[static_cast<std::size_t>(channel - 1)];
    ++generation_;
    matrices_[static_cast<std::size_t>(channel - 1)] = buildMatrix(slot.live(), generation_);
    persistLocked();
    if (listener_) listener_(channel);
}

bool ControlStore::patch(int channel, Json const& body, std::string& error)
{
    std::lock_guard lock(mu_);
    if (!validChannel(channel))
    {
        error = "unknown channel";
        return false;
    }
    auto& live = channels_[static_cast<std::size_t>(channel - 1)].live();
    if (!patchControls(live, body, error)) return false;
    publishLocked(channel);
    return true;
}

bool ControlStore::setBypass(int channel, bool enabled, std::string& error)
{
    Json body = Json::object();
    body["bypass"] = Json::boolean(enabled);
    return patch(channel, body, error);
}

bool ControlStore::setSlot(int channel, std::string const& slot, std::string& error)
{
    std::lock_guard lock(mu_);
    if (!validChannel(channel))
    {
        error = "unknown channel";
        return false;
    }
    auto& slots = channels_[static_cast<std::size_t>(channel - 1)];
    if (slot == "toggle") slots.active = slots.active == 'a' ? 'b' : 'a';
    else if (slot == "a" || slot == "b") slots.active = slot[0];
    else
    {
        error = "slot must be a, b or toggle";
        return false;
    }
    publishLocked(channel);
    return true;
}

bool ControlStore::reset(int channel, std::string const& control, std::string& error)
{
    std::lock_guard lock(mu_);
    if (!validChannel(channel))
    {
        error = "unknown channel";
        return false;
    }
    static char const* names[] = {"all", "white", "black", "gain", "pedestal", "brightness", "saturation", "bypass"};
    bool known = false;
    for (auto const* name : names)
    {
        if (control == name) known = true;
    }
    if (!known)
    {
        error = "unknown control";
        return false;
    }
    resetOne(channels_[static_cast<std::size_t>(channel - 1)].live(), control, defaultControls());
    publishLocked(channel);
    return true;
}

bool ControlStore::savePreset(int channel, std::string const& name, std::string& error)
{
    std::lock_guard lock(mu_);
    if (name.empty() || name.size() > 64)
    {
        error = "preset name is invalid";
        return false;
    }
    if (channel != 0 && !validChannel(channel))
    {
        error = "unknown channel";
        return false;
    }
    Preset preset;
    preset.name = name;
    preset.channel = channel;
    if (channel == 0)
    {
        for (auto const& slots : channels_) preset.channels.push_back(slots.live());
    }
    else
    {
        preset.channels.push_back(channels_[static_cast<std::size_t>(channel - 1)].live());
    }
    for (auto& existing : presets_)
    {
        if (existing.name == name && existing.channel == channel)
        {
            existing = preset;
            persistLocked();
            return true;
        }
    }
    presets_.push_back(preset);
    persistLocked();
    return true;
}

bool ControlStore::recallPreset(int channel, std::string const& name, std::string& error)
{
    std::lock_guard lock(mu_);
    if (channel != 0 && !validChannel(channel))
    {
        error = "unknown channel";
        return false;
    }
    for (auto const& preset : presets_)
    {
        if (preset.name != name || preset.channel != channel) continue;
        if (channel == 0)
        {
            if (preset.channels.size() != channels_.size())
            {
                error = "global preset does not match the channel count";
                return false;
            }
            for (std::size_t i = 0; i < channels_.size(); ++i)
            {
                channels_[i].live() = preset.channels[i];
                ++generation_;
                matrices_[i] = buildMatrix(channels_[i].live(), generation_);
            }
        }
        else
        {
            if (preset.channels.size() != 1)
            {
                error = "channel preset is empty";
                return false;
            }
            channels_[static_cast<std::size_t>(channel - 1)].live() = preset.channels[0];
            publishLocked(channel);
            return true;
        }
        persistLocked();
        if (listener_) listener_(0);
        return true;
    }
    error = "unknown preset";
    return false;
}

bool ControlStore::deletePreset(int channel, std::string const& name, std::string& error)
{
    std::lock_guard lock(mu_);
    for (auto it = presets_.begin(); it != presets_.end(); ++it)
    {
        if (it->name == name && it->channel == channel)
        {
            presets_.erase(it);
            persistLocked();
            return true;
        }
    }
    error = "unknown preset";
    return false;
}

Json ControlStore::exportPresets() const
{
    std::lock_guard lock(mu_);
    Json doc = Json::object();
    Json list = Json::array();
    for (auto const& preset : presets_)
    {
        Json obj = presetToJson(preset);
        obj["channel"] = Json::number(preset.channel);
        list.push(std::move(obj));
    }
    doc["presets"] = std::move(list);
    return doc;
}

bool ControlStore::importPresets(Json const& doc, std::string& error)
{
    std::lock_guard lock(mu_);
    Json const* list = doc.isArray() ? &doc : doc.find("presets");
    if (list == nullptr || !list->isArray())
    {
        error = "presets must be an array";
        return false;
    }
    std::vector<Preset> incoming;
    for (auto const& item : list->a)
    {
        Preset preset;
        int expect = 0;
        if (auto const* ch = item.find("channel"); ch != nullptr)
        {
            preset.channel = static_cast<int>(ch->num());
            expect = preset.channel == 0 ? static_cast<int>(channels_.size()) : 1;
        }
        if (!presetFromJson(item, preset, expect, error)) return false;
        if (preset.channel < 0 || preset.channel > static_cast<int>(channels_.size()))
        {
            error = "preset channel is out of range";
            return false;
        }
        incoming.push_back(std::move(preset));
    }
    presets_ = std::move(incoming);
    persistLocked();
    return true;
}

Json ControlStore::exportBundle() const
{
    std::lock_guard lock(mu_);
    Json doc = Json::object();
    Json channels = Json::array();
    for (auto const& slots : channels_)
    {
        Json item = Json::object();
        item["active"] = Json::str(slots.active == 'b' ? "b" : "a");
        item["a"] = controlsToJson(slots.a);
        item["b"] = controlsToJson(slots.b);
        channels.push(std::move(item));
    }
    doc["channels"] = std::move(channels);
    Json presets = Json::array();
    for (auto const& preset : presets_)
    {
        Json obj = presetToJson(preset);
        obj["channel"] = Json::number(preset.channel);
        presets.push(std::move(obj));
    }
    doc["presets"] = std::move(presets);
    return doc;
}

bool ControlStore::importBundle(Json const& doc, std::string& error)
{
    std::lock_guard lock(mu_);
    auto const* channels = doc.find("channels");
    if (channels == nullptr || !channels->isArray())
    {
        error = "channels must be an array";
        return false;
    }
    if (channels->a.size() != channels_.size())
    {
        error = "imported channel count does not match CC_CHANNELS";
        return false;
    }
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        auto const& item = channels->a[i];
        Controls a = defaultControls();
        Controls b = defaultControls();
        if (auto const* slot = item.find("a"))
        {
            if (!patchControls(a, *slot, error)) return false;
        }
        if (auto const* slot = item.find("b"))
        {
            if (!patchControls(b, *slot, error)) return false;
        }
        channels_[i].a = a;
        channels_[i].b = b;
        channels_[i].active = (item.find("active") && item.find("active")->text() == "b") ? 'b' : 'a';
        ++generation_;
        matrices_[i] = buildMatrix(channels_[i].live(), generation_);
    }
    if (auto const* presets = doc.find("presets"); presets && presets->isArray())
    {
        std::vector<Preset> incoming;
        for (auto const& item : presets->a)
        {
            Preset preset;
            int expect = 0;
            if (auto const* ch = item.find("channel"))
            {
                preset.channel = static_cast<int>(ch->num());
                expect = preset.channel == 0 ? static_cast<int>(channels_.size()) : 1;
            }
            if (!presetFromJson(item, preset, expect, error)) return false;
            incoming.push_back(std::move(preset));
        }
        presets_ = std::move(incoming);
    }
    persistLocked();
    if (listener_) listener_(0);
    return true;
}

void ControlStore::setListener(std::function<void(int)> listener)
{
    std::lock_guard lock(mu_);
    listener_ = std::move(listener);
}

std::string ControlStore::statePath() const
{
    return config_.stateDir + "/state.json";
}

void ControlStore::persistLocked()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(config_.stateDir, ec);
    if (ec)
    {
        logWarn("cannot create state directory " + config_.stateDir);
        return;
    }
    Json doc = Json::object();
    Json channels = Json::array();
    for (auto const& slots : channels_)
    {
        Json item = Json::object();
        item["active"] = Json::str(slots.active == 'b' ? "b" : "a");
        item["a"] = controlsToJson(slots.a);
        item["b"] = controlsToJson(slots.b);
        channels.push(std::move(item));
    }
    doc["channels"] = std::move(channels);
    Json presets = Json::array();
    for (auto const& preset : presets_)
    {
        Json obj = presetToJson(preset);
        obj["channel"] = Json::number(preset.channel);
        presets.push(std::move(obj));
    }
    doc["presets"] = std::move(presets);
    auto path = fs::path(statePath());
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out)
        {
            logWarn("cannot write " + tmp.string());
            return;
        }
        out << doc.dump();
    }
    fs::rename(tmp, path, ec);
    if (ec)
    {
        logWarn("cannot replace state file " + path.string());
    }
}

void ControlStore::loadLocked()
{
    std::ifstream in(statePath());
    if (!in) return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (buffer.str().empty()) return;
    Json doc;
    try
    {
        doc = parseJson(buffer.str());
    }
    catch (JsonError const& ex)
    {
        logWarn(std::string("ignoring unreadable state file: ") + ex.what());
        return;
    }
    if (auto const* channels = doc.find("channels"); channels && channels->isArray())
    {
        std::size_t const n = std::min(channels->a.size(), channels_.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const& item = channels->a[i];
            std::string error;
            if (auto const* a = item.find("a")) patchControls(channels_[i].a, *a, error);
            error.clear();
            if (auto const* b = item.find("b")) patchControls(channels_[i].b, *b, error);
            if (auto const* active = item.find("active"); active && active->text() == "b") channels_[i].active = 'b';
        }
    }
    if (auto const* presets = doc.find("presets"); presets && presets->isArray())
    {
        presets_.clear();
        for (auto const& item : presets->a)
        {
            Preset preset;
            std::string error;
            int expect = 0;
            if (auto const* ch = item.find("channel"))
            {
                preset.channel = static_cast<int>(ch->num());
                expect = preset.channel == 0 ? static_cast<int>(channels_.size()) : 1;
            }
            if (presetFromJson(item, preset, expect, error)) presets_.push_back(std::move(preset));
        }
    }
    logInfo("restored state from " + statePath());
}

} // namespace cc
