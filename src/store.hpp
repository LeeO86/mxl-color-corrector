#pragma once

#include "color/controls.hpp"
#include "color/process.hpp"
#include "config.hpp"
#include "util/json.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace cc
{

struct ChannelSlots
{
    Controls a{};
    Controls b{};
    char active = 'a';

    [[nodiscard]] Controls& live() { return active == 'b' ? b : a; }
    [[nodiscard]] Controls const& live() const { return active == 'b' ? b : a; }
};

struct Preset
{
    std::string name;
    int channel = 0; // 0 is a global preset
    std::vector<Controls> channels;
};

Json controlsToJson(Controls const& controls);
bool patchControls(Controls& controls, Json const& patch, std::string& error);

class ControlStore
{
public:
    explicit ControlStore(Config config);

    [[nodiscard]] Config const& config() const { return config_; }
    [[nodiscard]] int channels() const;

    [[nodiscard]] Controls live(int channel) const;
    [[nodiscard]] ChannelSlots slots(int channel) const;
    [[nodiscard]] FixedMatrix matrix(int channel) const;
    [[nodiscard]] char activeSlot(int channel) const;

    bool patch(int channel, Json const& body, std::string& error);
    bool setBypass(int channel, bool enabled, std::string& error);
    bool setSlot(int channel, std::string const& slot, std::string& error);
    bool reset(int channel, std::string const& control, std::string& error);

    bool savePreset(int channel, std::string const& name, std::string& error);
    bool recallPreset(int channel, std::string const& name, std::string& error);
    bool deletePreset(int channel, std::string const& name, std::string& error);
    [[nodiscard]] Json exportPresets() const;
    bool importPresets(Json const& doc, std::string& error);
    [[nodiscard]] Json exportBundle() const;
    bool importBundle(Json const& doc, std::string& error);

    void setListener(std::function<void(int channel)> listener);
    [[nodiscard]] std::string statePath() const;

private:
    bool validChannel(int channel) const;
    void publishLocked(int channel);
    void persistLocked();
    void loadLocked();
    Controls defaultControls() const;

    Config config_;
    mutable std::recursive_mutex mu_;
    std::vector<ChannelSlots> channels_;
    std::vector<FixedMatrix> matrices_;
    std::vector<Preset> presets_;
    std::uint64_t generation_ = 1;
    std::function<void(int)> listener_;
};

} // namespace cc
