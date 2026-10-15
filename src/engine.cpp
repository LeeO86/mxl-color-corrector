#include "engine.hpp"

#include "color/process.hpp"
#include "mode.hpp"
#include "mxl_source.hpp"
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
#include <sys/stat.h>
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

// This function's own output domain, and the domain scan. Sources are read on an MXL instance of
// each channel's own (SourceReader).
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

    // Creates this function's own domain. Does not rewrite an existing
    // domain_def.json or options.json. Returns nullptr and sets idMismatch
    // when a definition is already there for a different id.
    mxlInstance openOwn(std::string const& dir, std::string const& id, std::string const& label, std::uint64_t historyNs, bool& idMismatch)
    {
        idMismatch = false;
        std::lock_guard lock(mu_);
        if (auto it = open_.find(dir); it != open_.end()) return it->second;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto def = std::filesystem::path(dir) / "domain_def.json";
        if (std::filesystem::exists(def))
        {
            std::string existing;
            try
            {
                auto doc = parseJson(readFile(def));
                if (auto const* found = doc.find("id")) existing = found->text();
            }
            catch (JsonError const&)
            {
            }
            if (existing != id)
            {
                idMismatch = true;
                logError("domain_def.json in " + dir + " has id " + existing + " which does not match MXL_OUTPUT_DOMAIN_ID " + id);
                return nullptr;
            }
        }
        else
        {
            std::ofstream out(def);
            // BCP-007-03 requires id, label, description and tags.
            out << "{\"id\":\"" << id << "\",\"label\":\"" << label << "\",\"description\":\"Output domain of mxl-color-corrector\",\"tags\":{}}\n";
        }
        auto options = std::filesystem::path(dir) / "options.json";
        if (!std::filesystem::exists(options))
        {
            std::ofstream out(options);
            out << "{\"urn:x-mxl:option:history_duration/v1.0\": " << historyNs << "}\n";
        }
        auto* instance = mxlCreateInstance(dir.c_str(), nullptr);
        if (instance == nullptr) return nullptr;
        mxlGarbageCollectFlows(instance);
        open_[dir] = instance;
        return instance;
    }

    void closeAll()
    {
        std::lock_guard lock(mu_);
        for (auto& [_, instance] : open_)
        {
            if (instance) mxlDestroyInstance(instance);
        }
        open_.clear();
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
        bool mismatch = false;
        output = domains.openOwn(config.outputDomainDir, config.outputDomainId, "MXL Color Corrector", config.historyDurationNs, mismatch);
        if (mismatch)
        {
            throw std::runtime_error("domain_def.json id does not match MXL_OUTPUT_DOMAIN_ID");
        }
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
        std::string const ch = std::to_string(channel);
        std::string routeKey;
        SourceReader source;
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
        double grainCpu = 0;
        mxlGrainInfo outInfo{};
        std::uint8_t* outPayload = nullptr;
        double previewDue = 0;

        // A source flow deleted or re-created under the same id (MXL_ERR_FLOW_INVALID, or a stalled
        // head whose data file is gone or replaced) is opened again on a schedule, and the output
        // repeats its last grain until the new flow has a head. The output flow never stops.
        ReopenSchedule reopen;
        double lostAt = 0;
        bool resumePending = false;
        std::string sourceDir;     // the source domain's directory at the last open
        std::string watchPath;     // a missing flow's data file, checked every 20 ms between attempts
        double watchDue = 0;
        std::uint64_t lastOut = MXL_UNDEFINED_INDEX; // last whole output grain committed
        bool holding = false;
        std::vector<std::uint8_t> heldFrame;
        std::uint32_t heldFlags = 0;
        std::uint64_t heldGrains = 0;
        int holdLag = 0;
        std::uint64_t lastHead = MXL_UNDEFINED_INDEX;
        double headSeenAt = 0;
        double probeDue = 0;
        // Held grains catch up at most this many grains; a longer gap jumps to the current index.
        // A re-opened source does not move the output index back by less than this.
        constexpr std::uint64_t kHoldCatchUp = 8;

        auto closeReader = [&] {
            source.release();
            reader = nullptr;
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
            lastOut = MXL_UNDEFINED_INDEX;
            holding = false;
            heldFrame.clear();
        };
        auto releaseAll = [&] {
            closeReader();
            source.close();
            closeWriter();
            reopen.reset();
            resumePending = false;
            watchPath.clear();
        };
        // The last whole output grain, read back from this channel's own output flow.
        auto captureHeld = [&] {
            heldFrame.clear();
            if (writer == nullptr || output == nullptr || lastOut == MXL_UNDEFINED_INDEX) return;
            mxlFlowReader own = nullptr;
            if (mxlCreateFlowReader(output, writerFlow.c_str(), nullptr, &own) != MXL_STATUS_OK || own == nullptr) return;
            mxlGrainInfo grain{};
            std::uint8_t* payload = nullptr;
            if (mxlFlowReaderGetGrainNonBlocking(own, lastOut, &grain, &payload) == MXL_STATUS_OK && payload != nullptr)
            {
                heldFrame.assign(payload, payload + grain.grainSize);
                heldFlags = grain.flags;
            }
            mxlReleaseFlowReader(output, own);
        };
        // Repeats the held grain up to the current index (less the whole-grain offset).
        auto emitHeld = [&] {
            if (!holding || writer == nullptr || heldFrame.empty() || lastOut == MXL_UNDEFINED_INDEX || outOpen) return;
            mxlRational rate{current.rateNum, current.rateDen};
            auto const nowIndex = mxlGetCurrentIndex(&rate);
            auto const lag = static_cast<std::uint64_t>(std::max(holdLag, 0));
            if (nowIndex == MXL_UNDEFINED_INDEX || nowIndex < lag) return;
            auto const target = nowIndex - lag;
            if (target <= lastOut) return;
            for (auto next = target - lastOut > kHoldCatchUp ? target : lastOut + 1; next <= target; ++next)
            {
                mxlGrainInfo info{};
                std::uint8_t* payload = nullptr;
                if (mxlFlowWriterOpenGrain(writer, next, &info, &payload) != MXL_STATUS_OK || payload == nullptr) return;
                std::memcpy(payload, heldFrame.data(), std::min<std::size_t>(heldFrame.size(), info.grainSize));
                info.index = next;
                info.flags = heldFlags;
                info.validSlices = info.totalSlices;
                if (mxlFlowWriterCommitGrain(writer, &info) != MXL_STATUS_OK) return;
                lastOut = next;
                ++heldGrains;
            }
        };
        auto lostSource = [&](std::string const& route, std::string const& reason) {
            closeReader();
            if (outOpen && writer)
            {
                mxlFlowWriterCancelGrain(writer);
                outOpen = false;
            }
            produced = 0;
            grainCpu = 0;
            synced = false;
            if (reopen.active()) return;
            auto const now = monoNow();
            reopen.start(now);
            if (!resumePending)
            {
                lostAt = now;
                heldGrains = 0;
                captureHeld();
                holding = !heldFrame.empty();
            }
            resumePending = true;
            logInfo(std::string(reason == "read_error" ? "mxl_reader_error" : "mxl_flow_recreated") + " channel=" + ch + " source=" + route +
                    " reason=" + reason + (holding ? " holding last frame" : ""));
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
                auto const now = monoNow();
                int const idle = holding ? 3 : 0;
                if (reopen.active() && !reopen.due(now))
                {
                    // A flow that comes back between two attempts is opened at once.
                    bool appeared = false;
                    if (!watchPath.empty() && now >= watchDue)
                    {
                        watchDue = now + 0.02;
                        struct stat st{};
                        appeared = ::stat(watchPath.c_str(), &st) == 0;
                        if (appeared) watchPath.clear();
                    }
                    if (!appeared)
                    {
                        emitHeld();
                        runtime.setState(channel, idle, 0, 0);
                        auto const waitMs = std::clamp(static_cast<int>((reopen.next() - now) * 1000.0) + 1, 1, 5);
                        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                        continue;
                    }
                }
                auto map = domains.scan(config.scanPath);
                auto it = map.find(route.domainId);
                auto opened = it == map.end() ? SourceReader::Open::DomainMissing : source.open(it->second, route.flowId);
                VideoFormat format;
                if (opened == SourceReader::Open::Ok)
                {
                    auto const domainDir = std::filesystem::path(it->second);
                    auto text = readFile(domainDir / (route.flowId + ".mxl-flow") / "flow_def.json");
                    if (text.empty()) text = readFile(domainDir / route.flowId / "flow_def.json");
                    if (text.empty())
                    {
                        char buffer[8192];
                        std::size_t size = sizeof(buffer);
                        if (mxlGetFlowDef(source.instance(), route.flowId.c_str(), buffer, &size) == MXL_STATUS_OK) text.assign(buffer, buffer + std::strlen(buffer));
                    }
                    if (!parseFlow(text, format))
                    {
                        source.release();
                        opened = SourceReader::Open::FlowMissing;
                    }
                }
                if (opened != SourceReader::Open::Ok)
                {
                    runtime.setState(channel, idle, 0, 0);
                    if (reopen.active())
                    {
                        if (reopen.attempts() == 0)
                        {
                            char const* why = opened == SourceReader::Open::DomainMissing ? "domain_missing"
                                              : opened == SourceReader::Open::FlowMissing ? "flow_missing"
                                                                                          : "open_failed";
                            logInfo("mxl_flow_reopen_failed channel=" + ch + " source=" + key + " reason=" + why + ", retrying at 100, 200, 500 ms, then every 1 s");
                        }
                        reopen.failed(now);
                        watchPath.clear();
                        auto const dir = it != map.end() ? it->second : sourceDir;
                        if (!dir.empty())
                        {
                            auto const data = (std::filesystem::path(dir) / (route.flowId + ".mxl-flow") / "data").string();
                            struct stat st{};
                            if (::stat(data.c_str(), &st) != 0) watchPath = data;
                        }
                        watchDue = now + 0.02;
                        emitHeld();
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    backoffMs = std::min(2000, backoffMs * 2);
                    continue;
                }
                reader = source.reader();
                sourceDir = it->second;
                watchPath.clear();
                if (reopen.active())
                {
                    logInfo("mxl_flow_reopened channel=" + ch + " source=" + key + " attempts=" + std::to_string(reopen.attempts() + 1) +
                            " after_ms=" + std::to_string(static_cast<long>((now - lostAt) * 1000.0)));
                    runtime.addFlowReopen(channel);
                    reopen.reset();
                }
                lastHead = MXL_UNDEFINED_INDEX;
                headSeenAt = now;
                probeDue = 0;
                current = format;
                haveFormat = true;
                backoffMs = 50;
                auto flowId = uuidV5("mxl-color-corrector/" + config.nmosSeed + "/ch/" + ch + "/flow/" + std::to_string(format.width) + "x" +
                                     std::to_string(format.height) + "@" + std::to_string(format.rateNum) + "/" + std::to_string(format.rateDen) + "/" +
                                     format.interlace);
                if (writer == nullptr || writerFlow != flowId)
                {
                    closeWriter();
                    auto sourceId = uuidV5("mxl-color-corrector/" + config.nmosSeed + "/ch/" + ch + "/source");
                    auto label = "CC " + ch + " Video";
                    auto json = flowJson(flowId, label, "CC " + ch + ":Video", format, sourceId, nmos.deviceId());
                    bool created = false;
                    if (mxlCreateFlowWriter(output, json.c_str(), nullptr, &writer, nullptr, &created) != MXL_STATUS_OK || writer == nullptr)
                    {
                        logError("mxlCreateFlowWriter failed for channel " + ch);
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
                if (!warning.empty()) logWarn("channel " + ch + " " + warning);
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
                lostSource(key, "read_error");
                continue;
            }
            auto const head = info.runtime.headIndex;
            {
                // A writer that went away leaves the old flow's head standing; a read is then never
                // past the head, so MXL's own check never runs. Check while the head stands.
                auto const now = monoNow();
                if (head != lastHead)
                {
                    lastHead = head;
                    headSeenAt = now;
                }
                else if (now >= probeDue && now - headSeenAt > std::max(0.02, 2.0 * current.rateDen / std::max(current.rateNum, 1)))
                {
                    probeDue = now + 0.02;
                    if (!source.valid())
                    {
                        lostSource(key, head == MXL_UNDEFINED_INDEX ? "flow_replaced" : "flow_invalid");
                        continue;
                    }
                }
            }
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
                    grainCpu = 0;
                }
                logInfo("channel " + ch + " mode " + (decision.slice ? "slice" : std::string("whole-grain:") + fallbackName(decision.reason)));
                logged = decision.reason;
            }
            holdLag = decision.slice ? 0 : config.wholeGrainOffset;
            int const state = holding ? 3 : decision.slice ? 1 : 2;
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
                    emitHeld();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                index = decision.slice || head < static_cast<std::uint64_t>(config.wholeGrainOffset) ? head : head - static_cast<std::uint64_t>(config.wholeGrainOffset);
                // Never write an output grain again: the held grains may be ahead of the new head.
                if (lastOut != MXL_UNDEFINED_INDEX && lastOut >= index && lastOut - index < kHoldCatchUp) index = lastOut + 1;
                synced = true;
                if (resumePending)
                {
                    logInfo("mxl_flow_resumed channel=" + ch + " source=" + key + " after_ms=" + std::to_string(static_cast<long>((monoNow() - lostAt) * 1000.0)) +
                            " held_grains=" + std::to_string(heldGrains));
                    resumePending = false;
                }
                holding = false;
                heldFrame.clear();
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
            auto finishGrain = [&](std::uint8_t const* input, mxlGrainInfo const& grain, int row0, int row1) {
                auto t0 = monoNow();
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
                double const sliceSeconds = t1 - t0;
                grainCpu += sliceSeconds;
                bool const done = lines >= outInfo.totalSlices || invalid;
                if (!done)
                {
                    runtime.observeLatency(channel, sliceSeconds);
                }
                if (done)
                {
                    lastOut = index;
                    runtime.addGrain(channel, matrix.bypass, stats.clipped, stats.samples == 0 ? 1 : stats.samples, sliceSeconds, grainCpu, t1);
                    grainCpu = 0;
                    if (t1 >= previewDue && input && outPayload)
                    {
                        runtime.setPreview(channel, encodePreviewJpeg(input, current.width, current.height, lineBytes),
                            encodePreviewJpeg(outPayload, current.width, current.height, lineBytes));
                        previewDue = t1 + 1.0 / std::max(1, config.previewFps);
                    }
                }
                return true;
            };

            // A grain still too early while the head is past it will never come: the writer left it
            // unfinished, or never wrote it (a re-created flow starts at its own head, so the
            // whole-grain offset or the held output can point before its first grain). Skip it
            // rather than wait until the head is half a ring ahead.
            auto skipHole = [&](std::uint16_t want) {
                mxlFlowRuntimeInfo now{};
                if (mxlFlowReaderGetRuntimeInfo(reader, &now) != MXL_STATUS_OK || now.headIndex == MXL_UNDEFINED_INDEX || now.headIndex <= index) return;
                // The grain may have been finished just before the head moved on: read it once more.
                mxlGrainInfo again{};
                std::uint8_t* payload = nullptr;
                auto const status = want > 0 ? mxlFlowReaderGetGrainSliceNonBlocking(reader, index, want, &again, &payload)
                                             : mxlFlowReaderGetGrainNonBlocking(reader, index, &again, &payload);
                if (status != MXL_ERR_OUT_OF_RANGE_TOO_EARLY) return;
                if (outOpen)
                {
                    mxlFlowWriterCancelGrain(writer);
                    outOpen = false;
                }
                produced = 0;
                grainCpu = 0;
                ++index;
            };

            if (decision.slice)
            {
                std::uint16_t want = static_cast<std::uint16_t>(std::max(1, produced + 1));
                mxlGrainInfo grain{};
                std::uint8_t* payload = nullptr;
                auto status = mxlFlowReaderGetGrainSlice(reader, index, want, 2000000, &grain, &payload);
                if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT || status == MXL_ERR_NOT_READY)
                {
                    skipHole(want);
                    continue;
                }
                if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
                {
                    runtime.addLate(channel, 1);
                    if (outOpen)
                    {
                        mxlFlowWriterCancelGrain(writer);
                        outOpen = false;
                        grainCpu = 0;
                    }
                    synced = false;
                    continue;
                }
                if (status != MXL_STATUS_OK)
                {
                    lostSource(key, status == MXL_ERR_FLOW_INVALID ? "flow_invalid" : "read_error");
                    continue;
                }
                if (grain.validSlices < grain.totalSlices) sawPartial = true;
                else if (!sawPartial) ++completeOnly;
                int const rows = static_cast<int>(grain.validSlices) * std::max(linesPerSlice, 1);
                int const already = static_cast<int>(produced) * std::max(linesPerSlice, 1);
                if ((grain.flags & MXL_GRAIN_FLAG_INVALID) != 0 || rows >= current.height || grain.validSlices >= grain.totalSlices)
                {
                    int const from = (grain.flags & MXL_GRAIN_FLAG_INVALID) != 0 ? 0 : already;
                    if (!finishGrain(payload, grain, from, current.height))
                    {
                        closeReader();
                        continue;
                    }
                    produced = 0;
                    ++index;
                }
                else if (rows > already)
                {
                    if (!finishGrain(payload, grain, already, rows))
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
                auto status = mxlFlowReaderGetGrain(reader, index, 5000000, &grain, &payload);
                if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT || status == MXL_ERR_NOT_READY)
                {
                    skipHole(0);
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
                    lostSource(key, status == MXL_ERR_FLOW_INVALID ? "flow_invalid" : "read_error");
                    continue;
                }
                if (!finishGrain(payload, grain, 0, current.height))
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

void Engine::shutdown()
{
    if (!impl_) return;
    impl_->stop.store(true);
    for (auto& thread : impl_->threads)
    {
        if (thread.joinable()) thread.join();
    }
    impl_->threads.clear();
    impl_->domains.closeAll();
    impl_->output = nullptr;
}

} // namespace cc
