#include "metrics.hpp"

#include "mode.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace cc
{
namespace
{
char const* stateName(int state)
{
    if (state == 1) return "running";
    if (state == 2) return "whole_grain";
    return "waiting";
}
} // namespace

void Histogram::observe(double seconds)
{
    if (seconds < 0) seconds = 0;
    ++count_;
    sum_ += seconds;
    for (int i = 0; i < 8; ++i)
    {
        if (seconds <= kBounds[i])
        {
            ++buckets_[i];
            return;
        }
    }
    ++buckets_[8];
}

void Histogram::write(std::string& out, std::string const& name, std::string const& labels, std::string const& help) const
{
    out += "# HELP " + name + " " + help + "\n# TYPE " + name + " histogram\n";
    std::uint64_t cumulative = 0;
    auto emit = [&](std::string const& le, std::uint64_t value) {
        out += name + "_bucket{" + labels + ",le=\"" + le + "\"} " + std::to_string(value) + "\n";
    };
    static char const* les[] = {"0.0002", "0.0005", "0.001", "0.002", "0.005", "0.01", "0.02", "0.04", "+Inf"};
    for (int i = 0; i < 9; ++i)
    {
        cumulative += buckets_[i];
        emit(les[i], cumulative);
    }
    std::ostringstream sum;
    sum.setf(std::ios::fixed);
    sum.precision(6);
    sum << sum_;
    out += name + "_sum{" + labels + "} " + sum.str() + "\n";
    out += name + "_count{" + labels + "} " + std::to_string(count_) + "\n";
}

void ClipWindow::add(double seconds, std::uint64_t clipped, std::uint64_t total)
{
    samples_.push_back(Sample{seconds, clipped, total});
    while (!samples_.empty() && seconds - samples_.front().t > 1.0)
    {
        samples_.pop_front();
    }
}

double ClipWindow::ratio(double now) const
{
    std::uint64_t clipped = 0;
    std::uint64_t total = 0;
    for (auto const& sample : samples_)
    {
        if (now - sample.t <= 1.0)
        {
            clipped += sample.clipped;
            total += sample.total;
        }
    }
    if (total == 0) return 0;
    return static_cast<double>(clipped) / static_cast<double>(total);
}

RuntimeBoard::RuntimeBoard(int channels)
    : channels_(static_cast<std::size_t>(std::max(channels, 0)))
    , clips_(channels_.size())
{
}

void RuntimeBoard::setState(int channel, int state, int sliceMode, int fallback)
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    auto& row = channels_[static_cast<std::size_t>(channel - 1)];
    row.state = state;
    row.sliceMode = sliceMode;
    row.fallback = fallback;
}

void RuntimeBoard::setRoute(int channel, std::string const& label, std::string const& domain, std::string const& flow, std::string const& outputFlow,
    std::string const& warning, int width, int height, int rateNum, int rateDen, bool interlaced)
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    auto& row = channels_[static_cast<std::size_t>(channel - 1)];
    row.sourceLabel = label;
    row.domainId = domain;
    row.flowId = flow;
    row.outputFlowId = outputFlow;
    row.colorWarning = warning;
    row.width = width;
    row.height = height;
    row.rateNum = rateNum;
    row.rateDen = rateDen;
    row.interlaced = interlaced;
}

void RuntimeBoard::observeLatency(int channel, double seconds)
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    channels_[static_cast<std::size_t>(channel - 1)].latency.observe(seconds);
}

void RuntimeBoard::addGrain(int channel, bool bypass, std::uint64_t clipped, std::uint64_t samples, double latency, double processing, double now)
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    auto idx = static_cast<std::size_t>(channel - 1);
    auto& row = channels_[idx];
    row.grains += 1;
    row.bypass = bypass ? 1 : 0;
    row.latency.observe(latency);
    row.processing.observe(processing);
    clips_[idx].add(now, clipped, samples);
    row.clippedRatio = clips_[idx].ratio(now);
}

void RuntimeBoard::addLate(int channel, std::uint64_t count)
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    channels_[static_cast<std::size_t>(channel - 1)].late += count;
}

void RuntimeBoard::addSettingsChange(int channel)
{
    std::lock_guard lock(mu_);
    if (channel == 0)
    {
        for (auto& row : channels_) row.settingsChanges += 1;
        return;
    }
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    channels_[static_cast<std::size_t>(channel - 1)].settingsChanges += 1;
}

void RuntimeBoard::setPreview(int channel, std::vector<std::uint8_t> input, std::vector<std::uint8_t> output)
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    auto& row = channels_[static_cast<std::size_t>(channel - 1)];
    row.inputJpeg = std::move(input);
    row.outputJpeg = std::move(output);
}

ChannelStats RuntimeBoard::snapshot(int channel) const
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return {};
    return channels_[static_cast<std::size_t>(channel - 1)];
}

std::vector<std::uint8_t> RuntimeBoard::preview(int channel, bool output) const
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return {};
    auto const& row = channels_[static_cast<std::size_t>(channel - 1)];
    return output ? row.outputJpeg : row.inputJpeg;
}

std::string RuntimeBoard::render(double processCpuSeconds) const
{
    std::lock_guard lock(mu_);
    std::string out;
    out += "# HELP mxl_color_corrector_channel_state Channel state gauge, 1 for the active state\n";
    out += "# TYPE mxl_color_corrector_channel_state gauge\n";
    out += "# HELP mxl_color_corrector_grains_processed_total Grains committed\n# TYPE mxl_color_corrector_grains_processed_total counter\n";
    out += "# HELP mxl_color_corrector_slice_mode 1 when the channel is processing slice by slice\n# TYPE mxl_color_corrector_slice_mode gauge\n";
    out += "# HELP mxl_color_corrector_fallback_reason 1 for the active fallback reason\n# TYPE mxl_color_corrector_fallback_reason gauge\n";
    out += "# HELP mxl_color_corrector_late_grains_total Grains skipped because the reader fell behind\n# TYPE mxl_color_corrector_late_grains_total counter\n";
    out += "# HELP mxl_color_corrector_clipped_ratio Share of clipped samples in the last second\n# TYPE mxl_color_corrector_clipped_ratio gauge\n";
    out += "# HELP mxl_color_corrector_bypass 1 when the channel is bypassed\n# TYPE mxl_color_corrector_bypass gauge\n";
    out += "# HELP mxl_color_corrector_settings_changes_total Live control changes\n# TYPE mxl_color_corrector_settings_changes_total counter\n";
    out += "# HELP mxl_color_corrector_process_cpu_seconds_total Process CPU time\n# TYPE mxl_color_corrector_process_cpu_seconds_total counter\n";
    char const* states[] = {"waiting", "running", "whole_grain"};
    char const* reasons[] = {"none", "interlaced", "no_slice_commits", "slice_layout"};
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        auto const& row = channels_[i];
        auto const ch = std::to_string(i + 1);
        for (int s = 0; s < 3; ++s)
        {
            out += "mxl_color_corrector_channel_state{channel=\"" + ch + "\",state=\"" + states[s] + "\"} " +
                   std::to_string(row.state == s ? 1 : 0) + "\n";
        }
        out += "mxl_color_corrector_grains_processed_total{channel=\"" + ch + "\"} " + std::to_string(row.grains) + "\n";
        out += "mxl_color_corrector_slice_mode{channel=\"" + ch + "\"} " + std::to_string(row.sliceMode) + "\n";
        for (int r = 0; r < 4; ++r)
        {
            out += "mxl_color_corrector_fallback_reason{channel=\"" + ch + "\",reason=\"" + reasons[r] + "\"} " +
                   std::to_string(row.fallback == r ? 1 : 0) + "\n";
        }
        out += "mxl_color_corrector_late_grains_total{channel=\"" + ch + "\"} " + std::to_string(row.late) + "\n";
        std::ostringstream ratio;
        ratio.setf(std::ios::fixed);
        ratio.precision(6);
        ratio << row.clippedRatio;
        out += "mxl_color_corrector_clipped_ratio{channel=\"" + ch + "\"} " + ratio.str() + "\n";
        out += "mxl_color_corrector_bypass{channel=\"" + ch + "\"} " + std::to_string(row.bypass) + "\n";
        out += "mxl_color_corrector_settings_changes_total{channel=\"" + ch + "\"} " + std::to_string(row.settingsChanges) + "\n";
        auto labels = "channel=\"" + ch + "\"";
        row.latency.write(out, "mxl_color_corrector_added_latency_seconds", labels, "Input slice visible to output slice commit");
        row.processing.write(out, "mxl_color_corrector_processing_seconds", labels, "Colour processing time per grain");
        (void)stateName;
    }
    std::ostringstream cpu;
    cpu.setf(std::ios::fixed);
    cpu.precision(6);
    cpu << processCpuSeconds;
    out += "mxl_color_corrector_process_cpu_seconds_total " + cpu.str() + "\n";
    return out;
}

double processCpuSeconds()
{
    std::ifstream in("/proc/self/stat");
    std::string line;
    if (!std::getline(in, line)) return 0;
    auto close = line.rfind(')');
    if (close == std::string::npos) return 0;
    std::stringstream ss(line.substr(close + 2));
    std::string field;
    long utime = 0;
    long stime = 0;
    for (int i = 3; i <= 15 && ss >> field; ++i)
    {
        if (i == 14) utime = std::stol(field);
        if (i == 15) stime = std::stol(field);
    }
    long const ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return 0;
    return static_cast<double>(utime + stime) / static_cast<double>(ticks);
}

} // namespace cc
