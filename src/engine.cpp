#include "engine.hpp"

#include "color/process.hpp"
#include "hold.hpp"
#include "mode.hpp"
#include "mxl_source.hpp"
#include "preview.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "util/uuid.hpp"
#include "v210.hpp"
#include "video_format.hpp"

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
        GrainGeometry geo{}; // the grain layout of `current`: a field of an interlaced flow
        bool haveFormat = false;
        // A grain whose size or slice count does not match the flow's declared format (or a flow
        // whose grain rate does not) is never read or written past its end: the channel skips it
        // and reports `unsupported` until it processes a grain again.
        bool unsupported = false;
        double unsupportedLogDue = 0;
        std::uint64_t unsupportedOut = MXL_UNDEFINED_INDEX; // the last output grain counted as unsupported
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
        // head whose data file is gone or replaced) is opened again on a schedule. Meanwhile the
        // output repeats its last grain in its own timing (hold.hpp), black and flagged invalid
        // after CC_HOLD_MAX_S. Output indices only ever go up: MXL refuses a grain at or below the
        // last one committed.
        ReopenSchedule reopen;
        double lostAt = 0;
        bool resumePending = false;
        std::string sourceDir;     // the source domain's directory at the last open
        // While a flow cannot be opened, its files are looked at every 20 ms; when they change and
        // the flow is there, it is opened at once. The slower retries are for real open failures.
        std::string watchDir;      // the source domain's directory (scanned, or the last known)
        FlowFiles watchFiles;      // the flow's files at the last failed attempt
        double watchDue = 0;
        std::uint64_t lastOut = MXL_UNDEFINED_INDEX; // last whole output grain committed
        bool holding = false;
        hold::HeldPicture held;      // the last output grain, or of an interlaced flow the last field of each parity
        std::uint64_t heldGrains = 0;
        std::int64_t outPhase = 0;   // hold::phaseOf at the last committed output grain
        bool holdBlack = false;      // past CC_HOLD_MAX_S: black, flagged invalid
        bool phaseWait = false;      // the source is back but behind the held grains
        bool phaseWaitLogged = false;
        int writeFailures = 0;
        std::uint64_t lastHead = MXL_UNDEFINED_INDEX;
        double headSeenAt = 0;
        double probeDue = 0;
        // A source that stops writing without going away (a writer killed with its flow left
        // behind) is held too, until it writes again or a new writer replaces the flow.
        std::uint64_t lastWrite = 0;
        double writeSeenAt = 0;
        bool stallHold = false;

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
            stallHold = false;
            holdBlack = false;
            phaseWait = false;
            held.clear();
        };
        auto releaseAll = [&] {
            closeReader();
            source.close();
            closeWriter();
            reopen.reset();
            resumePending = false;
            watchDir.clear();
        };
        // Whether the flow's files changed since the last failed attempt and the flow is there.
        auto flowBack = [&](std::string const& flowId, double now) {
            if (watchDir.empty() || now < watchDue) return false;
            watchDue = now + 0.02;
            auto const files = flowFiles(watchDir, flowId);
            return files.present() && !(files == watchFiles);
        };
        auto watchFrom = [&](std::string const& dir, std::string const& flowId, double now) {
            watchDir = dir;
            watchFiles = flowFiles(watchDir, flowId);
            watchDue = now + 0.02;
        };
        // The last whole output grain, read back from this channel's own output flow; of an
        // interlaced flow also the one before it (the other field), so the held frame keeps both.
        auto captureHeld = [&] {
            held.clear();
            if (writer == nullptr || output == nullptr || lastOut == MXL_UNDEFINED_INDEX) return;
            mxlFlowReader own = nullptr;
            if (mxlCreateFlowReader(output, writerFlow.c_str(), nullptr, &own) != MXL_STATUS_OK || own == nullptr) return;
            auto keep = [&](std::uint64_t at) {
                mxlGrainInfo grain{};
                std::uint8_t* payload = nullptr;
                if (mxlFlowReaderGetGrainNonBlocking(own, at, &grain, &payload) == MXL_STATUS_OK && payload != nullptr)
                {
                    held.set(at, payload, grain.grainSize, grain.flags, geo.interlaced);
                }
            };
            keep(lastOut);
            if (geo.interlaced && !held.empty() && lastOut > 0) keep(lastOut - 1);
            mxlReleaseFlowReader(output, own);
        };
        // A writer that pauses up to this long and then catches up in sequence loses nothing;
        // a longer silence is held (a killed writer). A restart takes 1–2 s, so it is still bridged.
        // All in grains of the flow: fields of an interlaced flow (twice its declared frame rate).
        auto stallSeconds = [&] { return std::max(0.25, 10.0 * geo.rateDen / std::max(geo.rateNum, 1)); };
        auto catchUp = [&] { return hold::catchUpGrains(stallSeconds(), geo.rateNum, geo.rateDen); };
        // How many grains one fill may write (hold::fillCap): about the catch-up, at most 256 MB.
        auto fillCap = [&] { return hold::fillCap(catchUp(), held.grainBytes()); };
        // 3 holding, 4 no_signal (black after CC_HOLD_MAX_S), 0 waiting.
        auto holdState = [&] { return holding ? (holdBlack ? 4 : 3) : 0; };
        // Writes the held grain at [first, last].
        auto writeHeld = [&](std::uint64_t first, std::uint64_t last) {
            for (auto next = first; next <= last; ++next)
            {
                mxlGrainInfo info{};
                std::uint8_t* payload = nullptr;
                if (mxlFlowWriterOpenGrain(writer, next, &info, &payload) != MXL_STATUS_OK || payload == nullptr) return;
                auto const& grain = held.at(next);
                std::memcpy(payload, grain.data.data(), std::min<std::size_t>(grain.data.size(), info.grainSize));
                info.index = next;
                info.flags = grain.flags;
                info.validSlices = info.totalSlices;
                if (mxlFlowWriterCommitGrain(writer, &info) != MXL_STATUS_OK) return;
                lastOut = next;
                ++heldGrains;
            }
        };
        // Repeats the held grain in the output's own timing, up to now.
        auto emitHeld = [&] {
            if (!holding || writer == nullptr || held.empty() || lastOut == MXL_UNDEFINED_INDEX || outOpen) return;
            if (!holdBlack && hold::expired(monoNow(), lostAt, config.holdMaxS))
            {
                held.black(current.width, geo.lines, MXL_GRAIN_FLAG_INVALID);
                holdBlack = true;
                logWarn("mxl_hold_expired channel=" + ch + " source=" + routeKey + " after_s=" + std::to_string(config.holdMaxS) +
                        ", output black grains flagged invalid (no_signal)");
            }
            if (phaseWait) return;
            mxlRational rate{geo.rateNum, geo.rateDen};
            auto const range = hold::heldRange(mxlGetCurrentIndex(&rate), outPhase, lastOut, fillCap());
            writeHeld(range.first, range.last);
        };
        // Before output grain `next`: fill a gap after the last output grain with that grain's picture
        // (the output fell behind, a source grain was skipped, or the output resumes past the held
        // grains), so no output grain is left invalid.
        double gapLogDue = 0;
        auto fillGap = [&](std::uint64_t next) {
            if (lastOut == MXL_UNDEFINED_INDEX || next <= lastOut + 1 || writer == nullptr) return;
            if (held.empty()) captureHeld();
            if (held.empty()) return;
            auto const range = hold::fillRange(lastOut, next, fillCap());
            auto const gap = next - lastOut - 1;
            auto const t0 = monoNow();
            writeHeld(range.first, range.last);
            if (auto const now = monoNow(); now >= gapLogDue)
            {
                logInfo("output_gap_filled channel=" + ch + " gap=" + std::to_string(gap) + " grains=" + std::to_string(range.last - range.first + 1) +
                        " ms=" + std::to_string(static_cast<long>((now - t0) * 1000.0)) + " (repeated the last output grain, or field of the same parity)");
                gapLogDue = now + 5.0;
            }
        };
        // An output grain could not be opened or committed. Not the reader's fault: keep it, wait
        // a little and re-sync (which never goes at or below lastOut).
        auto outputFailed = [&](std::string const& route) {
            if (outOpen && writer)
            {
                mxlFlowWriterCancelGrain(writer);
                outOpen = false;
            }
            produced = 0;
            grainCpu = 0;
            synced = false;
            if (writeFailures++ % 250 == 0)
            {
                logWarn("output grain write failed channel=" + ch + " source=" + route + " index=" + std::to_string(index) + " last_out=" +
                        (lastOut == MXL_UNDEFINED_INDEX ? std::string("none") : std::to_string(lastOut)) + " failures=" + std::to_string(writeFailures));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        };
        // A grain (or flow) that does not match the declared format: counted (unless `count` is
        // false: another slice of a grain already counted), logged at most every 5 s, skipped.
        auto markUnsupported = [&](std::string const& route, std::string const& what, bool count = true) {
            if (outOpen && writer)
            {
                mxlFlowWriterCancelGrain(writer);
                outOpen = false;
            }
            produced = 0;
            grainCpu = 0;
            unsupported = true;
            if (count) runtime.addUnsupported(channel);
            if (auto const now = monoNow(); now >= unsupportedLogDue)
            {
                logWarn("unsupported_grain channel=" + ch + " source=" + route + " " + what + " expected lines=" + std::to_string(geo.lines) +
                        " bytes=" + std::to_string(geo.bytes) + " rate=" + std::to_string(geo.rateNum) + "/" + std::to_string(geo.rateDen) + " (" +
                        current.interlace + "), skipped");
                unsupportedLogDue = now + 5.0;
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
            stallHold = false;
            phaseWait = false;
            if (reopen.active()) return;
            auto const now = monoNow();
            reopen.start(now);
            if (!resumePending)
            {
                lostAt = now;
                heldGrains = 0;
                holdBlack = false;
                captureHeld();
                holding = !held.empty();
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
                int const idle = holdState();
                if (reopen.active() && !reopen.due(now))
                {
                    // A flow that comes back between two attempts is opened at once.
                    if (!flowBack(route.flowId, now))
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
                        watchFrom(it != map.end() ? it->second : sourceDir, route.flowId, now);
                        emitHeld();
                        continue;
                    }
                    // Not routed to a flow before (or a writer failure): back off, but open a flow
                    // of a known domain as soon as it shows up.
                    watchFrom(it != map.end() ? it->second : sourceDir, route.flowId, now);
                    auto const until = now + backoffMs / 1000.0;
                    while (!stop.load() && monoNow() < until && !flowBack(route.flowId, monoNow()))
                    {
                        auto const again = nmos.route(channel);
                        if (!again.enable || again.domainId + "/" + again.flowId != key) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    backoffMs = std::min(2000, backoffMs * 2);
                    continue;
                }
                reader = source.reader();
                sourceDir = it->second;
                watchDir.clear();
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
                lastWrite = 0;
                writeSeenAt = now;
                current = format;
                geo = grainGeometry(format);
                unsupported = false;
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
                    mxlFlowConfigInfo outConfig{};
                    if (mxlCreateFlowWriter(output, json.c_str(), nullptr, &writer, &outConfig, &created) != MXL_STATUS_OK || writer == nullptr)
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
                    published.interlaced = isInterlaced(format.interlace);
                    published.interlaceMode = published.interlaced ? format.interlace : "progressive";
                    published.colorspace = format.colorspace.empty() ? "BT709" : format.colorspace;
                    nmos.setOutput(channel, published);
                    logInfo(std::string(created ? "created" : "opened") + " output flow " + flowId + " " + std::to_string(format.width) + "x" +
                            std::to_string(format.height) + " " + format.interlace + " grain_rate " + std::to_string(outConfig.common.grainRate.numerator) + "/" +
                            std::to_string(outConfig.common.grainRate.denominator) + (geo.interlaced ? " (one field per grain, " + std::to_string(geo.lines) + " lines)" : ""));
                }
                std::string warning = isBt709(format.colorspace) ? "" : "flow colorimetry is " + format.colorspace + ", processing as BT.709";
                if (!warning.empty()) logWarn("channel " + ch + " " + warning);
                runtime.setRoute(channel, format.label.empty() ? route.flowId : format.label, route.domainId, route.flowId, writerFlow, warning, format.width,
                    format.height, format.rateNum, format.rateDen, isInterlaced(format.interlace));
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
            double const stallAfter = stallSeconds();
            {
                // A writer that went away leaves the old flow's head standing; a read is then never
                // past the head, so MXL's own check never runs. Check while the head stands.
                auto const now = monoNow();
                if (head != lastHead)
                {
                    lastHead = head;
                    headSeenAt = now;
                }
                else if (now >= probeDue && now - headSeenAt > std::max(0.02, 2.0 * geo.rateDen / std::max(geo.rateNum, 1)))
                {
                    probeDue = now + 0.02;
                    if (!source.valid())
                    {
                        lostSource(key, head == MXL_UNDEFINED_INDEX ? "flow_replaced" : "flow_invalid");
                        continue;
                    }
                }
                // Writes (slice commits too) are the source's progress; the head stands while a
                // slow writer fills a grain.
                if (info.runtime.lastWriteTime != lastWrite)
                {
                    lastWrite = info.runtime.lastWriteTime;
                    writeSeenAt = now;
                    if (stallHold)
                    {
                        // The same flow is written again: follow it from its head (the sync below
                        // ends the hold and never moves the output index back).
                        stallHold = false;
                        synced = false;
                    }
                }
                else if (synced && !holding && lastOut != MXL_UNDEFINED_INDEX && now - writeSeenAt > stallAfter)
                {
                    if (outOpen)
                    {
                        mxlFlowWriterCancelGrain(writer);
                        outOpen = false;
                    }
                    produced = 0;
                    grainCpu = 0;
                    captureHeld();
                    holding = !held.empty();
                    if (holding)
                    {
                        stallHold = true;
                        holdBlack = false;
                        lostAt = now;
                        heldGrains = 0;
                        resumePending = true;
                        logInfo("mxl_source_stalled channel=" + ch + " source=" + key + " no write for " +
                                std::to_string(static_cast<long>((now - writeSeenAt) * 1000.0)) + " ms, holding last frame");
                    }
                }
            }
            {
                // MXL indexes an interlaced flow by field (twice the declared rate). A flow whose
                // grain rate is not what its flow_def.json declares is not read.
                auto const rate = info.config.common.grainRate;
                if (static_cast<std::int64_t>(rate.numerator) * geo.rateDen != static_cast<std::int64_t>(geo.rateNum) * rate.denominator || rate.denominator == 0)
                {
                    // Counted once per source grain period. A hold that was running goes on (and
                    // turns black after CC_HOLD_MAX_S); its state is shown while it lasts.
                    markUnsupported(key, "flow grain_rate=" + std::to_string(rate.numerator) + "/" + std::to_string(rate.denominator));
                    emitHeld();
                    runtime.setState(channel, holding ? holdState() : 5, 0, static_cast<int>(logged));
                    auto const periodMs = rate.numerator > 0 ? 1000 * static_cast<std::int64_t>(rate.denominator) / rate.numerator : 20;
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<std::int64_t>(periodMs, 5, 100)));
                    continue;
                }
            }
            int const lineBytes = geo.lineBytes;
            auto const sliceBytes = info.config.discrete.sliceSizes[0];
            auto const totalHint = geo.lines > 0 && lineBytes > 0 && sliceBytes >= static_cast<std::uint32_t>(lineBytes) &&
                                            sliceBytes % static_cast<std::uint32_t>(lineBytes) == 0
                                        ? geo.lines / static_cast<int>(sliceBytes / static_cast<std::uint32_t>(lineBytes))
                                        : 0;
            bool const commits = sawPartial || completeOnly < 4;
            auto decision = decideMode(geo.interlaced, commits, geo.lines, totalHint, sliceBytes, lineBytes);
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
            int const state = holding ? holdState() : unsupported ? 5 : decision.slice ? 1 : 2;
            runtime.setState(channel, state, decision.slice ? 1 : 0, static_cast<int>(decision.reason));
            if (stallHold)
            {
                emitHeld();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
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
                // A new flow shows head 0 until its first grain (its flow info starts zeroed); hold on.
                if (head == MXL_UNDEFINED_INDEX || head == 0)
                {
                    emitHeld();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                auto const resume = hold::resumeAt(head, config.wholeGrainOffset, decision.slice, lastOut, catchUp());
                bool const writing = monoNow() - writeSeenAt < stallAfter;
                if (resume.action == hold::Action::Rephase && writing)
                {
                    // The source runs far behind the output (a writer indexing by UTC is 37 s behind
                    // TAI) and would never catch up; the output cannot go back. Make the output flow
                    // again in the source's timing; readers re-open a re-created flow.
                    logWarn("mxl_output_recreated channel=" + ch + " source=" + key + " lag_grains=" + std::to_string(resume.lag) +
                            ", the source runs behind the output; output flow made again in its timing");
                    closeReader();
                    closeWriter();
                    phaseWait = false;
                    phaseWaitLogged = false;
                    continue;
                }
                if (resume.action != hold::Action::Read)
                {
                    // A source a little behind the held grains is waited for without holding further,
                    // so it catches up; a source that does not write is held on.
                    phaseWait = writing && resume.wait();
                    if (phaseWait && !phaseWaitLogged)
                    {
                        logInfo("mxl_flow_resume_waiting channel=" + ch + " source=" + key + " head=" + std::to_string(head) + " last_out=" + std::to_string(lastOut) +
                                " lag_grains=" + std::to_string(resume.lag));
                        phaseWaitLogged = true;
                    }
                    if (!holding && !phaseWait)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        continue;
                    }
                    emitHeld();
                    int const waitState = holdState();
                    runtime.setState(channel, waitState == 0 ? (decision.slice ? 1 : 2) : waitState, decision.slice ? 1 : 0, static_cast<int>(decision.reason));
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                index = resume.index;
                phaseWait = false;
                phaseWaitLogged = false;
                synced = true;
                if (resumePending)
                {
                    logInfo("mxl_flow_resumed channel=" + ch + " source=" + key + " after_ms=" + std::to_string(static_cast<long>((monoNow() - lostAt) * 1000.0)) +
                            " held_grains=" + std::to_string(heldGrains));
                    resumePending = false;
                }
                // The held picture stays until the first real grain, for holes right after the resume.
                holding = false;
                holdBlack = false;
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
                    fillGap(index);
                    auto opened = mxlFlowWriterOpenGrain(writer, index, &outInfo, &outPayload);
                    if (opened != MXL_STATUS_OK || outPayload == nullptr)
                    {
                        return false;
                    }
                    outOpen = true;
                    produced = 0;
                    if (!grainFits(geo, outInfo.grainSize, outInfo.totalSlices))
                    {
                        // Slice mode opens the grain again for each slice: count it once.
                        bool const first = index != unsupportedOut;
                        unsupportedOut = index;
                        markUnsupported(key, "output grain_size=" + std::to_string(outInfo.grainSize) + " total_slices=" + std::to_string(outInfo.totalSlices),
                            first);
                        return true;
                    }
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
                    processV210(input, outPayload, current.width, geo.lines, lineBytes, lineBytes, row0, row1, matrix, &stats);
                }
                outInfo.index = index;
                outInfo.flags = grain.flags;
                std::uint16_t lines = invalid ? outInfo.totalSlices : static_cast<std::uint16_t>(std::min(geo.lines, row1));
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
                    unsupported = false;
                    if (!holding && !held.empty()) held.clear();
                    writeFailures = 0;
                    mxlRational rate{geo.rateNum, geo.rateDen};
                    outPhase = hold::phaseOf(index, mxlGetCurrentIndex(&rate));
                    runtime.addGrain(channel, matrix.bypass, stats.clipped, stats.samples == 0 ? 1 : stats.samples, sliceSeconds, grainCpu, t1);
                    grainCpu = 0;
                    if (t1 >= previewDue && input && outPayload)
                    {
                        // A field is shown at the frame's aspect (its lines doubled).
                        runtime.setPreview(channel, encodePreviewJpeg(input, current.width, geo.lines, lineBytes, 320, current.height),
                            encodePreviewJpeg(outPayload, current.width, geo.lines, lineBytes, 320, current.height));
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
                if (!grainFits(geo, grain.grainSize, grain.totalSlices))
                {
                    markUnsupported(key, "grain index=" + std::to_string(index) + " grain_size=" + std::to_string(grain.grainSize) +
                                             " total_slices=" + std::to_string(grain.totalSlices));
                    ++index;
                    continue;
                }
                if (grain.validSlices < grain.totalSlices) sawPartial = true;
                else if (!sawPartial) ++completeOnly;
                int const rows = static_cast<int>(grain.validSlices) * std::max(linesPerSlice, 1);
                int const already = static_cast<int>(produced) * std::max(linesPerSlice, 1);
                if ((grain.flags & MXL_GRAIN_FLAG_INVALID) != 0 || rows >= geo.lines || grain.validSlices >= grain.totalSlices)
                {
                    int const from = (grain.flags & MXL_GRAIN_FLAG_INVALID) != 0 ? 0 : already;
                    if (!finishGrain(payload, grain, from, geo.lines))
                    {
                        outputFailed(key);
                        continue;
                    }
                    produced = 0;
                    ++index;
                }
                else if (rows > already)
                {
                    if (!finishGrain(payload, grain, already, rows))
                    {
                        outputFailed(key);
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
                if (!grainFits(geo, grain.grainSize, grain.totalSlices))
                {
                    markUnsupported(key, "grain index=" + std::to_string(index) + " grain_size=" + std::to_string(grain.grainSize) +
                                             " total_slices=" + std::to_string(grain.totalSlices));
                    ++index;
                    continue;
                }
                if (!finishGrain(payload, grain, 0, geo.lines))
                {
                    outputFailed(key);
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
