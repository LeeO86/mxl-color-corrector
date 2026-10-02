#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace cc
{

class Histogram
{
public:
    void observe(double seconds);
    [[nodiscard]] std::uint64_t count() const { return count_; }
    [[nodiscard]] double sum() const { return sum_; }
    void write(std::string& out, std::string const& name, std::string const& labels, std::string const& help) const;

private:
    static constexpr double kBounds[8] = {0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.04};
    std::uint64_t buckets_[9]{};
    std::uint64_t count_ = 0;
    double sum_ = 0;
};

class ClipWindow
{
public:
    void add(double seconds, std::uint64_t clipped, std::uint64_t total);
    [[nodiscard]] double ratio(double now) const;

private:
    struct Sample
    {
        double t = 0;
        std::uint64_t clipped = 0;
        std::uint64_t total = 0;
    };
    std::deque<Sample> samples_;
};

struct ChannelStats
{
    int state = 0; // 0 waiting, 1 running, 2 whole-grain
    int sliceMode = 0;
    int fallback = 0;
    int bypass = 0;
    std::uint64_t grains = 0;
    std::uint64_t late = 0;
    std::uint64_t settingsChanges = 0;
    double clippedRatio = 0;
    std::string sourceLabel;
    std::string domainId;
    std::string flowId;
    std::string outputFlowId;
    std::string colorWarning;
    int width = 0;
    int height = 0;
    int rateNum = 0;
    int rateDen = 1;
    bool interlaced = false;
    Histogram latency;
    Histogram processing;
    std::vector<std::uint8_t> inputJpeg;
    std::vector<std::uint8_t> outputJpeg;
};

class RuntimeBoard
{
public:
    explicit RuntimeBoard(int channels);

    void setState(int channel, int state, int sliceMode, int fallback);
    void setRoute(int channel, std::string const& label, std::string const& domain, std::string const& flow, std::string const& outputFlow,
        std::string const& warning, int width, int height, int rateNum, int rateDen, bool interlaced);
    void observeLatency(int channel, double seconds);
    void addGrain(int channel, bool bypass, std::uint64_t clipped, std::uint64_t samples, double latency, double processing, double now);
    void addLate(int channel, std::uint64_t count);
    void addSettingsChange(int channel);
    void setPreview(int channel, std::vector<std::uint8_t> input, std::vector<std::uint8_t> output);

    [[nodiscard]] ChannelStats snapshot(int channel) const;
    [[nodiscard]] std::vector<std::uint8_t> preview(int channel, bool output) const;
    [[nodiscard]] std::string render(double processCpuSeconds) const;
    [[nodiscard]] int channels() const { return static_cast<int>(channels_.size()); }

private:
    mutable std::mutex mu_;
    std::vector<ChannelStats> channels_;
    std::vector<ClipWindow> clips_;
};

[[nodiscard]] double processCpuSeconds();

} // namespace cc
