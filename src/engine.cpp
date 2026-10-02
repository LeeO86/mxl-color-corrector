#include "engine.hpp"

#include "color/process.hpp"
#include "mode.hpp"
#include "preview.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "util/uuid.hpp"
#include "v210.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace cc
{
namespace
{
double monoNow()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

std::string readFile(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

struct VideoFormat
{
    int width = 1920;
    int height = 1080;
    int rateNum = 50;
    int rateDen = 1;
    std::string interlace = "progressive";
    std::string colorspace = "BT709";
    std::string media = "video/v210";
    std::string label;
    bool operator==(VideoFormat const& other) const
    {
        return width == other.width && height == other.height && rateNum == other.rateNum && rateDen == other.rateDen && interlace == other.interlace &&
               media == other.media;
    }
};

bool parseFlow(std::string const& text, VideoFormat& format)
{
    if (text.empty()) return false;
    try
    {
        auto doc = parseJson(text);
        if (auto const* w = doc.find("frame_width")) format.width = static_cast<int>(w->num());
        if (auto const* h = doc.find("frame_height")) format.height = static_cast<int>(h->num());
        if (auto const* rate = doc.find("grain_rate"); rate && rate->isObject())
        {
            if (auto const* n = rate->find("numerator")) format.rateNum = static_cast<int>(n->num());
            if (auto const* d = rate->find("denominator")) format.rateDen = std::max(1, static_cast<int>(d->num()));
        }
        if (auto const* mode = doc.find("interlace_mode")) format.interlace = mode->text();
        if (auto const* cs = doc.find("colorspace")) format.colorspace = cs->text();
        if (auto const* media = doc.find("media_type")) format.media = media->text();
        if (auto const* label = doc.find("label")) format.label = label->text();
        return format.width >= 2 && format.height >= 1 && format.media == "video/v210";
    }
    catch (JsonError const&)
    {
        return false;
    }
}

std::string flowJson(std::string const& id, std::string const& label, std::string const& group, VideoFormat const& format, std::string const& sourceId,
    std::string const& deviceId)
{
    std::ostringstream os;
    os << "{\n  \"id\": \"" << id << "\",\n  \"label\": \"" << label << "\",\n  \"description\": \"" << label
       << "\",\n  \"format\": \"urn:x-nmos:format:video\",\n  \"media_type\": \"video/v210\",\n"
       << "  \"tags\": {\"urn:x-nmos:tag:grouphint/v1.0\": [\"" << group << "\"]},\n"
       << "  \"grain_rate\": {\"numerator\": " << format.rateNum << ", \"denominator\": " << format.rateDen << "},\n"
       << "  \"frame_width\": " << format.width << ",\n  \"frame_height\": " << format.height << ",\n"
       << "  \"interlace_mode\": \"" << format.interlace << "\",\n  \"colorspace\": \"" << format.colorspace << "\",\n"
       << "  \"source_id\": \"" << sourceId << "\",\n  \"device_id\": \"" << deviceId << "\",\n  \"parents\": [],\n  \"components\": [\n"
       << "    {\"name\": \"Y\", \"width\": " << format.width << ", \"height\": " << format.height << ", \"bit_depth\": 10},\n"
       << "    {\"name\": \"Cb\", \"width\": " << format.width / 2 << ", \"height\": " << format.height << ", \"bit_depth\": 10},\n"
       << "    {\"name\": \"Cr\", \"width\": " << format.width / 2 << ", \"height\": " << format.height << ", \"bit_depth\": 10}\n  ]\n}\n";
    return os.str();
}

std::string lower(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool isBt709(std::string const& colorspace)
{
    auto text = lower(colorspace);
    return text.empty() || text.find("709") != std::string::npos;
}

class DomainCache
{
public:
    ~DomainCache()
    {
        for (auto& [_, instance] : open_)
        {
            if (instance) mxlDestroyInstance(instance);
        }
    }

    mxlInstance open(std::string const& dir, std::string const& id, std::string const& label)
    {
        std::lock_guard lock(mu_);
        if (auto it = open_.find(dir); it != open_.end()) return it->second;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto def = std::filesystem::path(dir) / "domain_def.json";
        if (!std::filesystem::exists(def))
        {
            std::ofstream out(def);
            out << "{\"id\":\"" << id << "\",\"label\":\"" << label << "\"}\n";
        }
        auto options = std::filesystem::path(dir) / "options.json";
        if (!std::filesystem::exists(options))
        {
            std::ofstream out(options);
            out << "{\"urn:x-mxl:option:history_duration/v1.0\": 200000000}\n";
        }
        auto* instance = mxlCreateInstance(dir.c_str(), nullptr);
        if (instance == nullptr) return nullptr;
        mxlGarbageCollectFlows(instance);
        open_[dir] = instance;
        return instance;
    }

    std::map<std::string, std::string> scan(std::string const& root) const
    {
        std::map<std::string, std::string> found;
        auto consider = [&](std::filesystem::path const& dir) {
            auto text = readFile(dir / "domain_def.json");
            if (text.empty()) return;
            try
            {
                auto doc = parseJson(text);
                if (auto const* id = doc.find("id"); id && isUuid(id->text())) found[id->text()] = dir.string();
            }
            catch (JsonError const&)
            {
            }
        };
        std::error_code ec;
        consider(root);
        for (auto const& entry : std::filesystem::directory_iterator(root, ec))
        {
            if (entry.is_directory()) consider(entry.path());
        }
        return found;
    }

private:
    std::mutex mu_;
    std::map<std::string, mxlInstance> open_;
};
} // namespace

struct Engine::Impl
{
    Config config;
    ControlStore& store;
    RuntimeBoard& runtime;
    NmosNode& nmos;
    std::atomic<bool> stop{false};
    DomainCache domains;
    mxlInstance output = nullptr;
    std::vector<std::thread> threads;

    Impl(Config cfg, ControlStore& storeIn, RuntimeBoard& runtimeIn, NmosNode& nmosIn)
        : config(std::move(cfg))
        , store(storeIn)
        , runtime(runtimeIn)
        , nmos(nmosIn)
    {
    }

    ~Impl()
    {
        stop.store(true);
        for (auto& thread : threads)
        {
            if (thread.joinable()) thread.join();
        }
    }

    void ensureOutput()
    {
        output = domains.open(config.outputDomainDir, config.outputDomainId, "MXL Color Corrector");
        if (output == nullptr)
        {
            throw std::runtime_error("mxlCreateInstance failed for " + config.outputDomainDir);
        }
        bool tmp = false;
        if (mxlIsTmpFs(config.outputDomainDir.c_str(), &tmp) == MXL_STATUS_OK && !tmp)
        {
            logWarn("MXL output domain is not on a tmpfs: " + config.outputDomainDir);
        }
        logInfo("output domain " + config.outputDomainDir + " id " + config.outputDomainId);
    }

    void runChannel(int channel)
    {
        std::string routeKey;
        mxlInstance sourceInstance = nullptr;
        mxlFlowReader reader = nullptr;
        mxlFlowWriter writer = nullptr;
        std::string writerFlow;
        VideoFormat current{};
        bool haveFormat = false;
        std::uint64_t index = 0;
        bool synced = false;
        int backoffMs = 50;
        int completeOnly = 0;
        bool sawPartial = false;
        FallbackReason logged = FallbackReason::None;
        bool outOpen = false;
        std::uint16_t produced = 0;
        mxlGrainInfo outInfo{};
        std::uint8_t* outPayload = nullptr;
        double previewDue = 0;

        auto closeReader = [&] {
            if (reader && sourceInstance)
            {
                mxlReleaseFlowReader(sourceInstance, reader);
                reader = nullptr;
            }
        };
        auto closeWriter = [&] {
            if (outOpen && writer)
            {
                mxlFlowWriterCancelGrain(writer);
                outOpen = false;
            }
            if (writer && output)
            {
                mxlReleaseFlowWriter(output, writer);
                writer = nullptr;
            }
        };
        auto releaseAll = [&] {
            closeReader();
            closeWriter();
        };

        while (!stop.load())
        {
            auto route = nmos.route(channel);
            if (!route.enable)
            {
                runtime.setState(channel, 0, 0, 0);
                releaseAll();
                routeKey.clear();
                synced = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            auto key = route.domainId + "/" + route.flowId;
            if (key != routeKey)
            {
                releaseAll();
                routeKey = key;
                synced = false;
                sawPartial = false;
                completeOnly = 0;
                haveFormat = false;
            }
            if (reader == nullptr)
            {
                auto map = domains.scan(config.scanPath);
                auto it = map.find(route.domainId);
                if (it == map.end())
                {
                    runtime.setState(channel, 0, 0, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    backoffMs = std::min(2000, backoffMs * 2);
                    continue;
                }
                sourceInstance = domains.open(it->second, route.domainId, "source");
                if (sourceInstance == nullptr)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    continue;
                }
                auto const domainDir = std::filesystem::path(it->second);
                auto text = readFile(domainDir / (route.flowId + ".mxl-flow") / "flow_def.json");
                if (text.empty()) text = readFile(domainDir / route.flowId / "flow_def.json");
                if (text.empty())
                {
                    char buffer[8192];
                    std::size_t size = sizeof(buffer);
                    if (mxlGetFlowDef(sourceInstance, route.flowId.c_str(), buffer, &size) == MXL_STATUS_OK) text.assign(buffer, buffer + std::strlen(buffer));
                }
                VideoFormat format;
                if (!parseFlow(text, format))
                {
                    runtime.setState(channel, 0, 0, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    backoffMs = std::min(2000, backoffMs * 2);
                    continue;
                }
                if (mxlCreateFlowReader(sourceInstance, route.flowId.c_str(), nullptr, &reader) != MXL_STATUS_OK || reader == nullptr)
                {
                    runtime.setState(channel, 0, 0, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    backoffMs = std::min(2000, backoffMs * 2);
                    continue;
                }
                current = format;
                haveFormat = true;
                backoffMs = 50;
                auto flowId = uuidV5("mxl-color-corrector/" + config.nmosSeed + "/ch/" + std::to_string(channel) + "/flow/" + std::to_string(format.width) + "x" +
                                     std::to_string(format.height) + "@" + std::to_string(format.rateNum) + "/" + std::to_string(format.rateDen) + "/" +
                                     format.interlace);
                if (writer == nullptr || writerFlow != flowId)
                {
                    closeWriter();
                    auto sourceId = uuidV5("mxl-color-corrector/" + config.nmosSeed + "/ch/" + std::to_string(channel) + "/source");
                    auto label = "CC " + std::to_string(channel) + " Video";
                    auto json = flowJson(flowId, label, "CC " + std::to_string(channel) + ":Video", format, sourceId, nmos.deviceId());
                    bool created = false;
                    if (mxlCreateFlowWriter(output, json.c_str(), nullptr, &writer, nullptr, &created) != MXL_STATUS_OK || writer == nullptr)
                    {
                        logError("mxlCreateFlowWriter failed for channel " + std::to_string(channel));
                        closeReader();
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        continue;
                    }
                    writerFlow = flowId;
                    OutputFormat published;
                    published.flowId = flowId;
                    published.width = format.width;
                    published.height = format.height;
                    published.rateNum = format.rateNum;
                    published.rateDen = format.rateDen;
                    published.interlaced = format.interlace != "progressive";
                    published.colorspace = format.colorspace.empty() ? "BT709" : format.colorspace;
                    nmos.setOutput(channel, published);
                    logInfo(std::string(created ? "created" : "opened") + " output flow " + flowId);
                }
                std::string warning = isBt709(format.colorspace) ? "" : "flow colorimetry is " + format.colorspace + ", processing as BT.709";
                if (!warning.empty()) logWarn("channel " + std::to_string(channel) + " " + warning);
                runtime.setRoute(channel, format.label.empty() ? route.flowId : format.label, route.domainId, route.flowId, writerFlow, warning, format.width,
                    format.height, format.rateNum, format.rateDen, format.interlace != "progressive");
            }
            if (!haveFormat || reader == nullptr || writer == nullptr)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            mxlFlowInfo info{};
            if (mxlFlowReaderGetInfo(reader, &info) != MXL_STATUS_OK)
            {
                closeReader();
                continue;
            }
            mxlFlowRuntimeInfo runtimeInfo{};
            mxlFlowReaderGetRuntimeInfo(reader, &runtimeInfo);
            auto const head = runtimeInfo.headIndex;
            int const lineBytes = v210Stride(current.width);
            auto const sliceBytes = info.config.discrete.sliceSizes[0];
            auto const totalHint = current.height > 0 && lineBytes > 0 && sliceBytes >= static_cast<std::uint32_t>(lineBytes) &&
                                            sliceBytes % static_cast<std::uint32_t>(lineBytes) == 0
                                        ? current.height / static_cast<int>(sliceBytes / static_cast<std::uint32_t>(lineBytes))
                                        : 0;
            bool const commits = sawPartial || completeOnly < 4;
            auto decision = decideMode(current.interlace != "progressive", commits, current.height, totalHint, sliceBytes, lineBytes);
            if (decision.reason != logged)
            {
                if (outOpen && writer)
                {
                    mxlFlowWriterCancelGrain(writer);
                    outOpen = false;
                    produced = 0;
                }
                logInfo("channel " + std::to_string(channel) + " mode " + (decision.slice ? "slice" : std::string("whole-grain:") + fallbackName(decision.reason)));
                logged = decision.reason;
            }
            int const state = decision.slice ? 1 : 2;
            runtime.setState(channel, state, decision.slice ? 1 : 0, static_cast<int>(decision.reason));
            auto const grainCount = std::max<std::uint32_t>(info.config.discrete.grainCount, 2);
            if (synced && head != MXL_UNDEFINED_INDEX && head > index && head - index > grainCount / 2)
            {
                runtime.addLate(channel, head - index);
                if (outOpen)
                {
                    mxlFlowWriterCancelGrain(writer);
                    outOpen = false;
                }
                index = decision.slice || head < static_cast<std::uint64_t>(config.wholeGrainOffset) ? head : head - static_cast<std::uint64_t>(config.wholeGrainOffset);
            }
            if (!synced)
            {
                if (head == MXL_UNDEFINED_INDEX)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                index = decision.slice || head < static_cast<std::uint64_t>(config.wholeGrainOffset) ? head : head - static_cast<std::uint64_t>(config.wholeGrainOffset);
                synced = true;
            }
            if (!decision.slice && head < index + static_cast<std::uint64_t>(std::max(config.wholeGrainOffset, 0)))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (decision.slice && head < index)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            auto matrix = store.matrix(channel);
            int const linesPerSlice = sliceBytes > 0 && lineBytes > 0 ? static_cast<int>(sliceBytes / static_cast<std::uint32_t>(lineBytes)) : 1;
            auto finishGrain = [&](std::uint8_t const* input, mxlGrainInfo const& grain, int row0, int row1, double waited) {
                if (!outOpen)
                {
                    auto opened = mxlFlowWriterOpenGrain(writer, index, &outInfo, &outPayload);
                    if (opened != MXL_STATUS_OK || outPayload == nullptr)
                    {
                        return false;
                    }
                    outOpen = true;
                    produced = 0;
                }
                auto t0 = monoNow();
                ProcessStats stats;
                bool invalid = (grain.flags & MXL_GRAIN_FLAG_INVALID) != 0;
                if (invalid)
                {
                    auto n = std::min<std::uint32_t>(grain.grainSize, outInfo.grainSize);
                    if (input && n > 0) std::memcpy(outPayload, input, n);
                }
                else if (input && outPayload)
                {
                    processV210(input, outPayload, current.width, current.height, lineBytes, lineBytes, row0, row1, matrix, &stats);
                }
                outInfo.index = index;
                outInfo.flags = grain.flags;
                std::uint16_t lines = invalid ? outInfo.totalSlices : static_cast<std::uint16_t>(std::min(current.height, row1));
                if (lines > outInfo.totalSlices) lines = outInfo.totalSlices;
                outInfo.validSlices = lines;
                auto committed = mxlFlowWriterCommitGrain(writer, &outInfo);
                auto t1 = monoNow();
                if (lines >= outInfo.totalSlices) outOpen = false;
                if (committed != MXL_STATUS_OK) return false;
                if (lines >= outInfo.totalSlices || invalid)
                {
                    runtime.addGrain(channel, matrix.bypass, stats.clipped, stats.samples == 0 ? 1 : stats.samples, t1 - waited, t1 - t0, t1);
                    if (t1 >= previewDue && input && outPayload)
                    {
                        runtime.setPreview(channel, encodePreviewJpeg(input, current.width, current.height, lineBytes),
                            encodePreviewJpeg(outPayload, current.width, current.height, lineBytes));
                        previewDue = t1 + 1.0 / std::max(1, config.previewFps);
                    }
                }
                return true;
            };

            if (decision.slice)
            {
                std::uint16_t want = static_cast<std::uint16_t>(std::max(1, produced + 1));
                mxlGrainInfo grain{};
                std::uint8_t* payload = nullptr;
                auto waited = monoNow();
                auto status = mxlFlowReaderGetGrainSlice(reader, index, want, 2000000, &grain, &payload);
                if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT || status == MXL_ERR_NOT_READY)
                {
                    continue;
                }
                if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
                {
                    runtime.addLate(channel, 1);
                    if (outOpen)
                    {
                        mxlFlowWriterCancelGrain(writer);
                        outOpen = false;
                    }
                    synced = false;
                    continue;
                }
                if (status != MXL_STATUS_OK)
                {
                    closeReader();
                    continue;
                }
                if (grain.validSlices < grain.totalSlices) sawPartial = true;
                else if (!sawPartial) ++completeOnly;
                int const rows = static_cast<int>(grain.validSlices) * std::max(linesPerSlice, 1);
                int const already = static_cast<int>(produced) * std::max(linesPerSlice, 1);
                if ((grain.flags & MXL_GRAIN_FLAG_INVALID) != 0 || rows >= current.height || grain.validSlices >= grain.totalSlices)
                {
                    if (!finishGrain(payload, grain, 0, current.height, waited))
                    {
                        closeReader();
                        continue;
                    }
                    produced = 0;
                    ++index;
                }
                else if (rows > already)
                {
                    if (!finishGrain(payload, grain, already, rows, waited))
                    {
                        closeReader();
                        continue;
                    }
                    produced = grain.validSlices;
                }
            }
            else
            {
                mxlGrainInfo grain{};
                std::uint8_t* payload = nullptr;
                auto waited = monoNow();
                auto status = mxlFlowReaderGetGrain(reader, index, 5000000, &grain, &payload);
                if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT || status == MXL_ERR_NOT_READY)
                {
                    continue;
                }
                if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
                {
                    runtime.addLate(channel, 1);
                    synced = false;
                    continue;
                }
                if (status != MXL_STATUS_OK)
                {
                    closeReader();
                    continue;
                }
                if (!finishGrain(payload, grain, 0, current.height, waited))
                {
                    closeReader();
                    continue;
                }
                ++index;
            }
        }
        releaseAll();
    }
};

Engine::Engine(Config config, ControlStore& store, RuntimeBoard& runtime, NmosNode& nmos)
    : impl_(std::make_unique<Impl>(std::move(config), store, runtime, nmos))
{
}

Engine::~Engine()
{
    stop();
}

void Engine::start()
{
    impl_->ensureOutput();
    for (int channel = 1; channel <= impl_->config.channels; ++channel)
    {
        VideoFormat format;
        auto flowId = uuidV5("mxl-color-corrector/" + impl_->config.nmosSeed + "/ch/" + std::to_string(channel) + "/flow/1920x1080@50/1/progressive");
        auto sourceId = uuidV5("mxl-color-corrector/" + impl_->config.nmosSeed + "/ch/" + std::to_string(channel) + "/source");
        auto label = "CC " + std::to_string(channel) + " Video";
        auto json = flowJson(flowId, label, "CC " + std::to_string(channel) + ":Video", format, sourceId, impl_->nmos.deviceId());
        mxlFlowWriter writer = nullptr;
        bool created = false;
        if (mxlCreateFlowWriter(impl_->output, json.c_str(), nullptr, &writer, nullptr, &created) == MXL_STATUS_OK && writer)
        {
            mxlReleaseFlowWriter(impl_->output, writer);
        }
        OutputFormat published;
        published.flowId = flowId;
        published.width = 1920;
        published.height = 1080;
        published.rateNum = 50;
        published.rateDen = 1;
        impl_->nmos.setOutput(channel, published);
        impl_->threads.emplace_back([this, channel] { impl_->runChannel(channel); });
    }
}

void Engine::stop()
{
    if (!impl_) return;
    impl_->stop.store(true);
}

} // namespace cc
