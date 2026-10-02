#include "api.hpp"

#include "mode.hpp"
#include "util/json.hpp"

#include <optional>

namespace cc
{
namespace
{
std::vector<std::string> parts(std::string path)
{
    if (!path.empty() && path.back() == '/') path.pop_back();
    std::vector<std::string> out;
    std::string cur;
    for (char c : path)
    {
        if (c == '/')
        {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::optional<int> channelNumber(std::string const& text)
{
    if (text.empty()) return std::nullopt;
    int value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

void jsonError(HttpResponse& res, int status, std::string const& error)
{
    res.status = status;
    res.contentType = "application/json";
    Json body = Json::object();
    body["error"] = Json::str(error);
    res.body = body.dump();
}

Json channelView(ControlStore& store, RuntimeBoard& runtime, NmosNode& nmos, int channel)
{
    auto controls = store.live(channel);
    auto slots = store.slots(channel);
    auto stats = runtime.snapshot(channel);
    auto route = nmos.route(channel);
    Json obj = controlsToJson(controls);
    obj["channel"] = Json::number(channel);
    obj["label"] = Json::str("CC " + std::to_string(channel));
    obj["ab"] = Json::str(slots.active == 'b' ? "b" : "a");
    char const* state = stats.state == 1 ? "running" : stats.state == 2 ? "whole_grain" : "waiting";
    obj["state"] = Json::str(state);
    obj["slice_mode"] = Json::boolean(stats.sliceMode != 0);
    obj["fallback_reason"] = Json::str(fallbackName(static_cast<FallbackReason>(stats.fallback)));
    obj["clipped_ratio"] = Json::number(stats.clippedRatio);
    obj["grains"] = Json::number(static_cast<double>(stats.grains));
    obj["source_label"] = Json::str(stats.sourceLabel);
    obj["source_domain"] = Json::str(route.domainId);
    obj["source_flow"] = Json::str(route.flowId);
    obj["output_flow"] = Json::str(stats.outputFlowId);
    obj["color_warning"] = Json::str(stats.colorWarning);
    obj["width"] = Json::number(stats.width);
    obj["height"] = Json::number(stats.height);
    obj["rate_num"] = Json::number(stats.rateNum);
    obj["rate_den"] = Json::number(stats.rateDen);
    obj["interlaced"] = Json::boolean(stats.interlaced);
    return obj;
}

Json statusDocument(Services const& services)
{
    Json doc = Json::object();
    doc["service"] = Json::str("mxl-color-corrector");
    doc["channels"] = Json::array();
    int n = services.store->channels();
    for (int i = 1; i <= n; ++i) doc["channels"].push(channelView(*services.store, *services.runtime, *services.nmos, i));
    doc["nmos"] = services.nmos->summary();
    return doc;
}
} // namespace

std::string eventsHello(Services const& services)
{
    Json doc = statusDocument(services);
    doc["type"] = Json::str("state");
    return doc.dump();
}

void handleEvent(Services const& services, std::string const& text)
{
    Json msg;
    try
    {
        msg = parseJson(text);
    }
    catch (JsonError const&)
    {
        return;
    }
    auto type = msg.find("type") ? msg.find("type")->text() : std::string();
    if (type != "patch" && type != "bypass" && type != "ab" && type != "reset") return;
    int channel = msg.find("channel") ? static_cast<int>(msg.find("channel")->num()) : 0;
    std::string error;
    if (type == "patch" && msg.find("controls")) services.store->patch(channel, *msg.find("controls"), error);
    else if (type == "bypass") services.store->setBypass(channel, msg.find("enabled") && msg.find("enabled")->isBool() && msg.find("enabled")->b, error);
    else if (type == "ab") services.store->setSlot(channel, msg.find("slot") ? msg.find("slot")->text() : "toggle", error);
    else if (type == "reset") services.store->reset(channel, msg.find("control") ? msg.find("control")->text() : "all", error);
}

void dispatchHttp(Services const& services, HttpRequest const& req, HttpResponse& res)
{
    if (req.method == "OPTIONS")
    {
        res.status = 204;
        res.body.clear();
        return;
    }
    auto path = req.path;
    if (!path.empty() && path.back() == '/') path.pop_back();
    if (services.serveNmos && (path == "/x-nmos" || path.rfind("/x-nmos/", 0) == 0))
    {
        services.nmos->handle(req, res);
        return;
    }
    if (path == "/livez")
    {
        res.contentType = "text/plain";
        res.body = "ok\n";
        return;
    }
    if (path == "/readyz")
    {
        res.contentType = "text/plain";
        res.body = "ok\n";
        return;
    }
    if (path == "/metrics")
    {
        res.contentType = "text/plain; version=0.0.4";
        res.body = services.runtime->render(processCpuSeconds());
        return;
    }
    if (path == "/statusz" || path == "/api/v1/status")
    {
        res.body = statusDocument(services).dump();
        return;
    }
    if (path == "/api/v1/nmos")
    {
        res.body = services.nmos->summary().dump();
        return;
    }
    if (path == "/api/v1/settings")
    {
        auto const& cfg = services.store->config();
        Json obj = Json::object();
        obj["channels"] = Json::number(cfg.channels);
        obj["scan_path"] = Json::str(cfg.scanPath);
        obj["output_domain_dir"] = Json::str(cfg.outputDomainDir);
        obj["output_domain_id"] = Json::str(cfg.outputDomainId);
        obj["clip"] = Json::str(clipModeName(cfg.clip));
        obj["rgb_clip"] = Json::boolean(cfg.rgbClip);
        obj["whole_grain_offset"] = Json::number(cfg.wholeGrainOffset);
        obj["nmos_registry"] = Json::str(cfg.nmosRegistryAddress);
        obj["nmos_port"] = Json::number(cfg.nmosPort);
        obj["web_port"] = Json::number(cfg.webPort);
        obj["seed"] = Json::str(cfg.nmosSeed);
        obj["state_dir"] = Json::str(cfg.stateDir);
        res.body = obj.dump();
        return;
    }
    if (path == "/api/v1/presets/export" || path == "/api/v1/presets")
    {
        if (req.method == "GET")
        {
            res.body = services.store->exportPresets().dump();
            return;
        }
    }
    if (path == "/api/v1/presets/import" && (req.method == "POST" || req.method == "PUT"))
    {
        try
        {
            auto doc = parseJson(req.body);
            std::string error;
            if (!services.store->importPresets(doc, error)) jsonError(res, 400, error);
            else res.body = services.store->exportPresets().dump();
        }
        catch (JsonError const& ex)
        {
            jsonError(res, 400, ex.what());
        }
        return;
    }

    auto seg = parts(path);
    if (seg.size() >= 3 && seg[0] == "api" && seg[1] == "v1" && seg[2] == "channels" && seg.size() == 3 && req.method == "GET")
    {
        Json list = Json::array();
        for (int i = 1; i <= services.store->channels(); ++i) list.push(channelView(*services.store, *services.runtime, *services.nmos, i));
        res.body = list.dump();
        return;
    }
    if (seg.size() >= 4 && seg[0] == "api" && seg[1] == "v1" && seg[2] == "channels")
    {
        auto number = channelNumber(seg[3]);
        if (!number || *number < 1 || *number > services.store->channels())
        {
            jsonError(res, 404, "unknown channel");
            return;
        }
        int const channel = *number;
        if (seg.size() == 4 && req.method == "GET")
        {
            res.body = channelView(*services.store, *services.runtime, *services.nmos, channel).dump();
            return;
        }
        if (seg.size() == 5 && seg[4] == "controls")
        {
            if (req.method == "GET")
            {
                auto view = channelView(*services.store, *services.runtime, *services.nmos, channel);
                res.body = view.dump();
                return;
            }
            if (req.method == "PATCH" || req.method == "PUT")
            {
                try
                {
                    auto body = parseJson(req.body.empty() ? "{}" : req.body);
                    std::string error;
                    if (!services.store->patch(channel, body, error)) jsonError(res, 400, error);
                    else res.body = channelView(*services.store, *services.runtime, *services.nmos, channel).dump();
                }
                catch (JsonError const& ex)
                {
                    jsonError(res, 400, ex.what());
                }
                return;
            }
        }
        if (seg.size() == 5 && seg[4] == "bypass" && req.method == "POST")
        {
            bool enabled = true;
            if (!req.body.empty())
            {
                try
                {
                    auto body = parseJson(req.body);
                    if (auto const* flag = body.find("enabled"); flag && flag->isBool()) enabled = flag->b;
                }
                catch (JsonError const& ex)
                {
                    jsonError(res, 400, ex.what());
                    return;
                }
            }
            std::string error;
            if (!services.store->setBypass(channel, enabled, error)) jsonError(res, 400, error);
            else res.body = channelView(*services.store, *services.runtime, *services.nmos, channel).dump();
            return;
        }
        if (seg.size() == 5 && seg[4] == "ab" && req.method == "POST")
        {
            std::string slot = "toggle";
            if (!req.body.empty())
            {
                try
                {
                    auto body = parseJson(req.body);
                    if (auto const* value = body.find("slot")) slot = value->text();
                }
                catch (JsonError const& ex)
                {
                    jsonError(res, 400, ex.what());
                    return;
                }
            }
            std::string error;
            if (!services.store->setSlot(channel, slot, error)) jsonError(res, 400, error);
            else res.body = channelView(*services.store, *services.runtime, *services.nmos, channel).dump();
            return;
        }
        if (seg.size() == 5 && seg[4] == "reset" && req.method == "POST")
        {
            std::string control = "all";
            if (!req.body.empty())
            {
                try
                {
                    auto body = parseJson(req.body);
                    if (auto const* value = body.find("control")) control = value->text();
                }
                catch (JsonError const& ex)
                {
                    jsonError(res, 400, ex.what());
                    return;
                }
            }
            std::string error;
            if (!services.store->reset(channel, control, error)) jsonError(res, 400, error);
            else res.body = channelView(*services.store, *services.runtime, *services.nmos, channel).dump();
            return;
        }
        if (seg.size() >= 5 && seg[4] == "presets")
        {
            if (seg.size() == 5 && req.method == "GET")
            {
                Json list = Json::array();
                auto all = services.store->exportPresets();
                if (auto const* presets = all.find("presets"))
                {
                    for (auto const& item : presets->a)
                    {
                        if (auto const* ch = item.find("channel"); ch && static_cast<int>(ch->num()) == channel) list.push(item);
                    }
                }
                res.body = list.dump();
                return;
            }
            if (seg.size() == 5 && req.method == "POST")
            {
                try
                {
                    auto body = parseJson(req.body);
                    auto name = body.find("name") ? body.find("name")->text() : std::string();
                    std::string error;
                    if (!services.store->savePreset(channel, name, error)) jsonError(res, 400, error);
                    else res.body = services.store->exportPresets().dump();
                }
                catch (JsonError const& ex)
                {
                    jsonError(res, 400, ex.what());
                }
                return;
            }
            if (seg.size() == 7 && seg[6] == "recall" && req.method == "POST")
            {
                std::string error;
                if (!services.store->recallPreset(channel, seg[5], error)) jsonError(res, 404, error);
                else res.body = channelView(*services.store, *services.runtime, *services.nmos, channel).dump();
                return;
            }
            if (seg.size() == 6 && req.method == "DELETE")
            {
                std::string error;
                if (!services.store->deletePreset(channel, seg[5], error)) jsonError(res, 404, error);
                else res.body = "{\"deleted\":true}";
                return;
            }
        }
        if (seg.size() == 6 && seg[4] == "preview" && req.method == "GET")
        {
            bool output = seg[5] == "output.jpg" || seg[5] == "output";
            bool input = seg[5] == "input.jpg" || seg[5] == "input";
            if (!output && !input)
            {
                jsonError(res, 404, "unknown preview");
                return;
            }
            auto bytes = services.runtime->preview(channel, output);
            if (bytes.empty())
            {
                jsonError(res, 404, "no preview yet");
                return;
            }
            res.status = 200;
            res.contentType = "image/jpeg";
            res.body.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
            return;
        }
    }
    if (seg.size() >= 3 && seg[0] == "api" && seg[1] == "v1" && seg[2] == "presets")
    {
        if (seg.size() == 3 && req.method == "POST")
        {
            try
            {
                auto body = parseJson(req.body);
                auto name = body.find("name") ? body.find("name")->text() : std::string();
                std::string error;
                if (!services.store->savePreset(0, name, error)) jsonError(res, 400, error);
                else res.body = services.store->exportPresets().dump();
            }
            catch (JsonError const& ex)
            {
                jsonError(res, 400, ex.what());
            }
            return;
        }
        if (seg.size() == 5 && seg[4] == "recall" && req.method == "POST")
        {
            std::string error;
            if (!services.store->recallPreset(0, seg[3], error)) jsonError(res, 404, error);
            else res.body = statusDocument(services).dump();
            return;
        }
        if (seg.size() == 4 && req.method == "DELETE")
        {
            std::string error;
            if (!services.store->deletePreset(0, seg[3], error)) jsonError(res, 404, error);
            else res.body = "{\"deleted\":true}";
            return;
        }
    }
    if (req.method == "GET" && services.ui != nullptr && (path.empty() || path == "/" || path.find("/api/") != 0))
    {
        if (path.empty() || path == "/" || path == "/index.html")
        {
            res.status = 200;
            res.contentType = "text/html; charset=utf-8";
            res.body = *services.ui;
            return;
        }
    }
    jsonError(res, 404, "not found");
}

} // namespace cc
