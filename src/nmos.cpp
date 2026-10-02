#include "nmos.hpp"

#include "util/log.hpp"
#include "util/uuid.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <sstream>

namespace cc
{
namespace
{
std::string escape(std::string const& text)
{
    std::string out;
    for (char c : text)
    {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string httpExchange(std::string const& host, int port, std::string const& method, std::string const& path, std::string const& body)
{
    if (host.empty()) return {};
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || res == nullptr) return {};
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        freeaddrinfo(res);
        return {};
    }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    {
        ::close(fd);
        freeaddrinfo(res);
        return {};
    }
    freeaddrinfo(res);
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0)
    {
        ::close(fd);
        return {};
    }
    std::string out;
    char buf[2048];
    while (true)
    {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
    }
    ::close(fd);
    return out;
}

std::string labelFor(int channel)
{
    return "CC " + std::to_string(channel);
}

bool readBool(Json const* node, bool& dest)
{
    if (node == nullptr) return false;
    if (node->isBool())
    {
        dest = node->b;
        return true;
    }
    return false;
}
} // namespace

NmosNode::NmosNode(Config config)
    : config_(std::move(config))
{
    nodeId_ = uuidV5("mxl-color-corrector/" + config_.nmosSeed + "/node");
    deviceId_ = uuidV5("mxl-color-corrector/" + config_.nmosSeed + "/device");
    channels_.resize(static_cast<std::size_t>(config_.channels));
    for (int i = 0; i < config_.channels; ++i)
    {
        auto& channel = channels_[static_cast<std::size_t>(i)];
        auto base = "mxl-color-corrector/" + config_.nmosSeed + "/ch/" + std::to_string(i + 1);
        channel.receiverId = uuidV5(base + "/receiver");
        channel.sourceId = uuidV5(base + "/source");
        channel.senderId = uuidV5(base + "/sender");
        channel.format.width = 1920;
        channel.format.height = 1080;
        channel.format.rateNum = 50;
        channel.format.rateDen = 1;
        channel.format.flowId = uuidV5(base + "/flow/1920x1080@50/1/progressive");
        channel.flowId = channel.format.flowId;
    }
}

NmosNode::~NmosNode()
{
    stop();
}

void NmosNode::start()
{
    if (config_.nmosDnsSd)
    {
        logWarn("NMOS_DNS_SD is set; this build uses the static registry only");
    }
    stop_ = false;
    if (!config_.nmosRegistryAddress.empty())
    {
        registry_ = std::thread([this] { registryLoop(); });
    }
}

void NmosNode::stop()
{
    stop_ = true;
    if (registry_.joinable()) registry_.join();
}

std::string NmosNode::version() const
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
    return std::to_string(sec) + ":" + std::to_string(ns);
}

NmosRoute NmosNode::route(int channel) const
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return {};
    auto const& leg = channels_[static_cast<std::size_t>(channel - 1)].active;
    NmosRoute route;
    route.enable = leg.master && !leg.flowId.empty();
    route.domainId = leg.domainId;
    route.flowId = leg.flowId;
    route.senderId = leg.senderId;
    return route;
}

void NmosNode::setOutput(int channel, OutputFormat format)
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return;
    auto& slot = channels_[static_cast<std::size_t>(channel - 1)];
    slot.format = std::move(format);
    slot.flowId = slot.format.flowId;
}

OutputFormat NmosNode::output(int channel) const
{
    std::lock_guard lock(mu_);
    if (channel < 1 || channel > static_cast<int>(channels_.size())) return {};
    return channels_[static_cast<std::size_t>(channel - 1)].format;
}

void NmosNode::activate(Channel& channel)
{
    channel.active = channel.staged;
}

Json NmosNode::summary() const
{
    std::lock_guard lock(mu_);
    Json obj = Json::object();
    obj["node_id"] = Json::str(nodeId_);
    obj["device_id"] = Json::str(deviceId_);
    obj["registered"] = Json::boolean(registered_);
    obj["registry"] = Json::str(config_.nmosRegistryAddress);
    obj["port"] = Json::number(config_.nmosPort);
    obj["dns_sd"] = Json::boolean(config_.nmosDnsSd);
    Json receivers = Json::array();
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        auto const& channel = channels_[i];
        Json row = Json::object();
        row["channel"] = Json::number(static_cast<double>(i + 1));
        row["receiver_id"] = Json::str(channel.receiverId);
        row["sender_id"] = Json::str(channel.senderId);
        row["flow_id"] = Json::str(channel.flowId);
        row["master_enable"] = Json::boolean(channel.active.master);
        row["mxl_domain_id"] = channel.active.domainId.empty() ? Json::nul() : Json::str(channel.active.domainId);
        row["mxl_flow_id"] = channel.active.flowId.empty() ? Json::nul() : Json::str(channel.active.flowId);
        row["subscription_sender"] = channel.active.senderId.empty() ? Json::nul() : Json::str(channel.active.senderId);
        receivers.push(std::move(row));
    }
    obj["channels"] = std::move(receivers);
    return obj;
}

std::string NmosNode::receiverJson(Channel const& channel, std::string const& version) const
{
    int index = 1;
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        if (channels_[i].receiverId == channel.receiverId) index = static_cast<int>(i + 1);
    }
    auto label = labelFor(index) + " Video";
    std::ostringstream os;
    os << "{\"id\":\"" << channel.receiverId << "\",\"version\":\"" << version << "\",\"label\":\"" << escape(label)
       << "\",\"description\":\"MXL colour corrector video receiver\",\"format\":\"urn:x-nmos:format:video\","
       << "\"caps\":{\"media_types\":[\"video/v210\"],\"constraint_sets\":[{\"urn:x-nmos:cap:format:media_type\":{\"enum\":[\"video/v210\"]},"
       << "\"urn:x-nmos:cap:format:grain_rate\":{\"enum\":[{\"numerator\":24000,\"denominator\":1001},{\"numerator\":24,\"denominator\":1},"
       << "{\"numerator\":25,\"denominator\":1},{\"numerator\":30000,\"denominator\":1001},{\"numerator\":30,\"denominator\":1},"
       << "{\"numerator\":50,\"denominator\":1},{\"numerator\":60000,\"denominator\":1001},{\"numerator\":60,\"denominator\":1}]},"
       << "\"urn:x-nmos:cap:format:frame_width\":{\"minimum\":2,\"maximum\":3840},"
       << "\"urn:x-nmos:cap:format:frame_height\":{\"minimum\":2,\"maximum\":2160},"
       << "\"urn:x-nmos:cap:format:color_sampling\":{\"enum\":[\"YCbCr-4:2:2\"]},"
       << "\"urn:x-nmos:cap:format:component_depth\":{\"enum\":[10]},"
       << "\"urn:x-nmos:cap:format:interlace_mode\":{\"enum\":[\"progressive\",\"interlaced_tff\",\"interlaced_bff\",\"interlaced_psf\"]}}],"
       << "\"version\":\"" << version << "\"},\"device_id\":\"" << deviceId_
       << "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"interface_bindings\":[],\"subscription\":{\"sender_id\":"
       << (channel.active.senderId.empty() ? "null" : "\"" + channel.active.senderId + "\"") << ",\"active\":"
       << ((channel.active.master && !channel.active.flowId.empty()) ? "true" : "false") << "},\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\""
       << escape(labelFor(index) + ":Video") << "\"]}}";
    return os.str();
}

std::string NmosNode::senderJson(Channel const& channel, std::string const& version) const
{
    int index = 1;
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        if (channels_[i].senderId == channel.senderId) index = static_cast<int>(i + 1);
    }
    auto label = labelFor(index) + " Video";
    std::ostringstream os;
    os << "{\"id\":\"" << channel.senderId << "\",\"version\":\"" << version << "\",\"label\":\"" << escape(label)
       << "\",\"description\":\"MXL colour corrector video sender\",\"flow_id\":\"" << channel.flowId
       << "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"device_id\":\"" << deviceId_
       << "\",\"manifest_href\":null,\"interface_bindings\":[],\"subscription\":{\"receiver_id\":null,\"active\":true},"
       << "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" << escape(labelFor(index) + ":Video") << "\"]}}";
    return os.str();
}

std::string NmosNode::connectionJson(bool sender, Channel const& channel, Leg const& leg, std::string const& version, bool staged) const
{
    (void)version;
    std::ostringstream os;
    if (sender)
    {
        os << "{\"sender_id\":\"" << channel.senderId << "\",\"master_enable\":true,\"activation\":{\"mode\":"
           << (staged ? "null" : "\"activate_immediate\"") << ",\"requested_time\":null,\"activation_time\":null},"
           << "\"receiver_id\":null,\"transport_file\":{\"data\":null,\"type\":null},\"transport_params\":[{\"mxl_domain_id\":\""
           << config_.outputDomainId << "\",\"mxl_flow_id\":\"" << channel.flowId << "\"}]}";
    }
    else
    {
        os << "{\"receiver_id\":\"" << channel.receiverId << "\",\"master_enable\":" << (leg.master ? "true" : "false")
           << ",\"activation\":{\"mode\":" << (staged ? "null" : "\"activate_immediate\"")
           << ",\"requested_time\":null,\"activation_time\":null},\"sender_id\":"
           << (leg.senderId.empty() ? "null" : "\"" + leg.senderId + "\"") << ",\"transport_file\":{\"data\":null,\"type\":null},"
           << "\"transport_params\":[{\"mxl_domain_id\":" << (leg.domainId.empty() ? "null" : "\"" + leg.domainId + "\"") << ",\"mxl_flow_id\":"
           << (leg.flowId.empty() ? "null" : "\"" + leg.flowId + "\"") << "}]}";
    }
    return os.str();
}

bool NmosNode::postRegistry(std::string const& body) const
{
    auto response = httpExchange(config_.nmosRegistryAddress, config_.nmosRegistryPort, "POST", "/x-nmos/registration/v1.3/resource", body);
    return response.find(" 200 ") != std::string::npos || response.find(" 201 ") != std::string::npos;
}

void NmosNode::registryLoop()
{
    while (!stop_)
    {
        std::string node;
        std::string device;
        std::vector<std::string> resources;
        {
            std::lock_guard lock(mu_);
            auto v = version();
            auto href = "http://" + config_.hostId + ":" + std::to_string(config_.nmosPort);
            node = std::string("{\"type\":\"node\",\"data\":{\"id\":\"") + nodeId_ + "\",\"version\":\"" + v + "\",\"label\":\"" + escape(config_.hostId) +
                   "\",\"description\":\"MXL Color Corrector\",\"href\":\"" + href + "/x-nmos/node/v1.3/self\",\"hostname\":\"" + escape(config_.hostId) +
                   "\",\"api\":{\"versions\":[\"v1.3\"],\"endpoints\":[{\"host\":\"" + escape(config_.hostId) + "\",\"port\":" +
                   std::to_string(config_.nmosPort) + ",\"protocol\":\"http\"}]},\"caps\":{},\"services\":[],\"clocks\":[{\"name\":\"clk0\",\"ref_type\":\"internal\"}],\"tags\":{}}}";
            std::string senders = "[";
            std::string receivers = "[";
            for (std::size_t i = 0; i < channels_.size(); ++i)
            {
                if (i)
                {
                    senders += ",";
                    receivers += ",";
                }
                senders += "\"" + channels_[i].senderId + "\"";
                receivers += "\"" + channels_[i].receiverId + "\"";
                auto const& channel = channels_[i];
                auto label = labelFor(static_cast<int>(i + 1)) + " Video";
                auto interlace = channel.format.interlaced ? "interlaced_tff" : "progressive";
                resources.push_back("{\"type\":\"source\",\"data\":{\"id\":\"" + channel.sourceId + "\",\"version\":\"" + v + "\",\"label\":\"" + escape(label) +
                                     "\",\"description\":\"Corrected video\",\"format\":\"urn:x-nmos:format:video\",\"caps\":{},\"device_id\":\"" + deviceId_ +
                                     "\",\"parents\":[],\"clock_name\":\"clk0\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" +
                                     escape(labelFor(static_cast<int>(i + 1)) + ":Video") + "\"]}}}");
                resources.push_back("{\"type\":\"flow\",\"data\":{\"id\":\"" + channel.flowId + "\",\"version\":\"" + v + "\",\"label\":\"" + escape(label) +
                                     "\",\"description\":\"Corrected video\",\"format\":\"urn:x-nmos:format:video\",\"media_type\":\"video/v210\",\"source_id\":\"" +
                                     channel.sourceId + "\",\"device_id\":\"" + deviceId_ + "\",\"parents\":[],\"grain_rate\":{\"numerator\":" +
                                     std::to_string(channel.format.rateNum) + ",\"denominator\":" + std::to_string(channel.format.rateDen) +
                                     "},\"frame_width\":" + std::to_string(channel.format.width) + ",\"frame_height\":" + std::to_string(channel.format.height) +
                                     ",\"interlace_mode\":\"" + interlace + "\",\"colorspace\":\"" + escape(channel.format.colorspace) +
                                     "\",\"components\":[{\"name\":\"Y\",\"width\":" + std::to_string(channel.format.width) + ",\"height\":" +
                                     std::to_string(channel.format.height) + ",\"bit_depth\":10},{\"name\":\"Cb\",\"width\":" +
                                     std::to_string(channel.format.width / 2) + ",\"height\":" + std::to_string(channel.format.height) +
                                     ",\"bit_depth\":10},{\"name\":\"Cr\",\"width\":" + std::to_string(channel.format.width / 2) + ",\"height\":" +
                                     std::to_string(channel.format.height) + ",\"bit_depth\":10}],\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" +
                                     escape(labelFor(static_cast<int>(i + 1)) + ":Video") + "\"]}}}");
                resources.push_back(std::string("{\"type\":\"sender\",\"data\":") + senderJson(channel, v) + "}");
                resources.push_back(std::string("{\"type\":\"receiver\",\"data\":") + receiverJson(channel, v) + "}");
            }
            senders += "]";
            receivers += "]";
            device = std::string("{\"type\":\"device\",\"data\":{\"id\":\"") + deviceId_ + "\",\"version\":\"" + v +
                     "\",\"label\":\"MXL Color Corrector\",\"description\":\"Live RGB gain and pedestal corrector\",\"type\":\"urn:x-nmos:device:generic\","
                     "\"node_id\":\"" +
                     nodeId_ + "\",\"senders\":" + senders + ",\"receivers\":" + receivers + ",\"controls\":[],\"tags\":{}}}";
        }
        bool ok = postRegistry(node) && postRegistry(device);
        for (auto const& resource : resources) ok = postRegistry(resource) && ok;
        auto health = httpExchange(config_.nmosRegistryAddress, config_.nmosRegistryPort, "POST", "/x-nmos/registration/v1.3/health/nodes/" + nodeId_, "");
        ok = ok && (health.find(" 200 ") != std::string::npos || health.find(" 204 ") != std::string::npos);
        {
            std::lock_guard lock(mu_);
            registered_ = ok;
        }
        for (int i = 0; i < 50 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void NmosNode::handle(HttpRequest const& req, HttpResponse& res)
{
    if (req.method == "OPTIONS")
    {
        res.status = 204;
        res.body.clear();
        return;
    }
    std::lock_guard lock(mu_);
    auto path = req.path;
    if (!path.empty() && path.back() == '/') path.pop_back();
    auto v = version();
    auto base = "http://" + config_.hostId + ":" + std::to_string(config_.nmosPort);
    auto text = [&](std::string body) {
        res.status = 200;
        res.contentType = "application/json";
        res.body = std::move(body);
    };
    if (path == "/x-nmos" || path.empty())
    {
        text("[{\"name\":\"node\",\"href\":\"" + base + "/x-nmos/node/\"},{\"name\":\"connection\",\"href\":\"" + base + "/x-nmos/connection/\"}]");
        return;
    }
    if (path == "/x-nmos/node")
    {
        text("[\"v1.3/\"]");
        return;
    }
    if (path == "/x-nmos/connection")
    {
        text("[\"v1.1/\"]");
        return;
    }
    if (path == "/x-nmos/node/v1.3")
    {
        text("[\"self/\",\"sources/\",\"flows/\",\"devices/\",\"senders/\",\"receivers/\"]");
        return;
    }
    if (path == "/x-nmos/connection/v1.1")
    {
        text("[\"single/\"]");
        return;
    }
    if (path == "/x-nmos/connection/v1.1/single")
    {
        text("[\"senders/\",\"receivers/\"]");
        return;
    }
    if (path == "/x-nmos/node/v1.3/self")
    {
        text("{\"id\":\"" + nodeId_ + "\",\"version\":\"" + v + "\",\"label\":\"" + escape(config_.hostId) +
             "\",\"description\":\"MXL Color Corrector\",\"hostname\":\"" + escape(config_.hostId) + "\",\"href\":\"" + base +
             "/x-nmos/node/v1.3/self\",\"api\":{\"versions\":[\"v1.3\"],\"endpoints\":[{\"host\":\"" + escape(config_.hostId) + "\",\"port\":" +
             std::to_string(config_.nmosPort) + ",\"protocol\":\"http\"}]},\"caps\":{},\"services\":[],\"clocks\":[{\"name\":\"clk0\",\"ref_type\":\"internal\"}],\"tags\":{}}");
        return;
    }
    if (path == "/x-nmos/node/v1.3/devices" || path == "/x-nmos/node/v1.3/devices/" + deviceId_)
    {
        std::string senders;
        std::string receivers;
        for (std::size_t i = 0; i < channels_.size(); ++i)
        {
            if (i)
            {
                senders += ",";
                receivers += ",";
            }
            senders += "\"" + channels_[i].senderId + "\"";
            receivers += "\"" + channels_[i].receiverId + "\"";
        }
        auto device = std::string("{\"id\":\"") + deviceId_ + "\",\"version\":\"" + v +
                      "\",\"label\":\"MXL Color Corrector\",\"description\":\"Live RGB gain and pedestal corrector\","
                      "\"type\":\"urn:x-nmos:device:generic\",\"node_id\":\"" +
                      nodeId_ + "\",\"senders\":[" + senders + "],\"receivers\":[" + receivers + "],\"controls\":[],\"tags\":{}}";
        if (path.find(deviceId_) != std::string::npos) text(device);
        else text("[" + device + "]");
        return;
    }
    auto list = [&](auto&& one) {
        std::string body = "[";
        for (std::size_t i = 0; i < channels_.size(); ++i)
        {
            if (i) body += ",";
            body += one(channels_[i]);
        }
        body += "]";
        text(body);
    };
    if (path == "/x-nmos/node/v1.3/receivers")
    {
        list([&](Channel const& channel) { return receiverJson(channel, v); });
        return;
    }
    if (path == "/x-nmos/node/v1.3/senders")
    {
        list([&](Channel const& channel) { return senderJson(channel, v); });
        return;
    }
    if (path == "/x-nmos/node/v1.3/sources")
    {
        list([&](Channel const& channel) {
            int index = 1;
            for (std::size_t i = 0; i < channels_.size(); ++i)
                if (channels_[i].sourceId == channel.sourceId) index = static_cast<int>(i + 1);
            return std::string("{\"id\":\"") + channel.sourceId + "\",\"version\":\"" + v + "\",\"label\":\"" + escape(labelFor(index) + " Video") +
                   "\",\"description\":\"Corrected video\",\"format\":\"urn:x-nmos:format:video\",\"caps\":{},\"device_id\":\"" + deviceId_ +
                   "\",\"parents\":[],\"clock_name\":\"clk0\",\"tags\":{}}";
        });
        return;
    }
    if (path == "/x-nmos/node/v1.3/flows")
    {
        list([&](Channel const& channel) {
            auto interlace = channel.format.interlaced ? "interlaced_tff" : "progressive";
            return std::string("{\"id\":\"") + channel.flowId + "\",\"version\":\"" + v + "\",\"label\":\"Corrected video\",\"description\":\"Corrected video\","
                   "\"format\":\"urn:x-nmos:format:video\",\"media_type\":\"video/v210\",\"source_id\":\"" +
                   channel.sourceId + "\",\"device_id\":\"" + deviceId_ + "\",\"parents\":[],\"grain_rate\":{\"numerator\":" +
                   std::to_string(channel.format.rateNum) + ",\"denominator\":" + std::to_string(channel.format.rateDen) + "},\"frame_width\":" +
                   std::to_string(channel.format.width) + ",\"frame_height\":" + std::to_string(channel.format.height) + ",\"interlace_mode\":\"" + interlace +
                   "\",\"colorspace\":\"" + escape(channel.format.colorspace) + "\",\"tags\":{}}";
        });
        return;
    }

    auto match = [&](std::string const& prefix, bool sender) -> Channel* {
        if (path.rfind(prefix, 0) != 0) return nullptr;
        auto rest = path.substr(prefix.size());
        auto slash = rest.find('/');
        auto id = slash == std::string::npos ? rest : rest.substr(0, slash);
        for (auto& channel : channels_)
        {
            if ((sender && channel.senderId == id) || (!sender && channel.receiverId == id)) return &channel;
        }
        return nullptr;
    };
    std::string const recvPrefix = "/x-nmos/connection/v1.1/single/receivers/";
    std::string const sendPrefix = "/x-nmos/connection/v1.1/single/senders/";
    if (path == "/x-nmos/connection/v1.1/single/receivers")
    {
        list([&](Channel const& channel) {
            return std::string("{\"id\":\"") + channel.receiverId + "\",\"device_id\":\"" + deviceId_ +
                   "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"interface_bindings\":[]}";
        });
        return;
    }
    if (path == "/x-nmos/connection/v1.1/single/senders")
    {
        list([&](Channel const& channel) {
            return std::string("{\"id\":\"") + channel.senderId + "\",\"device_id\":\"" + deviceId_ +
                   "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"interface_bindings\":[]}";
        });
        return;
    }
    if (auto* channel = match(recvPrefix, false))
    {
        auto rest = path.substr(recvPrefix.size() + channel->receiverId.size());
        if (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
        if (rest.empty())
        {
            text(std::string("{\"id\":\"") + channel->receiverId + "\",\"device_id\":\"" + deviceId_ +
                 "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"interface_bindings\":[]}");
            return;
        }
        if (rest == "constraints")
        {
            text("[{\"master_enable\":{},\"mxl_domain_id\":{},\"mxl_flow_id\":{}}]");
            return;
        }
        if (rest == "transporttype")
        {
            text("\"urn:x-nmos:transport:mxl\"");
            return;
        }
        if (rest == "active")
        {
            text(connectionJson(false, *channel, channel->active, v, false));
            return;
        }
        if (rest == "staged")
        {
            if (req.method == "PATCH" || req.method == "PUT")
            {
                Json body;
                try
                {
                    body = req.body.empty() ? Json::object() : parseJson(req.body);
                }
                catch (JsonError const&)
                {
                    res.status = 400;
                    res.body = "{\"error\":\"invalid json\"}";
                    return;
                }
                bool master = channel->staged.master;
                if (readBool(body.find("master_enable"), master)) channel->staged.master = master;
                if (auto const* sender = body.find("sender_id"))
                {
                    channel->staged.senderId = sender->isNull() ? std::string() : sender->text();
                    if (!channel->staged.senderId.empty() && !isUuid(channel->staged.senderId))
                    {
                        res.status = 400;
                        res.body = "{\"error\":\"sender_id must be a UUID\"}";
                        return;
                    }
                }
                if (auto const* params = body.find("transport_params"); params && params->isArray() && !params->a.empty())
                {
                    auto const& leg = params->a[0];
                    if (auto const* domain = leg.find("mxl_domain_id"))
                    {
                        channel->staged.domainId = domain->isNull() ? std::string() : domain->text();
                        if (!channel->staged.domainId.empty() && !isUuid(channel->staged.domainId))
                        {
                            res.status = 400;
                            res.body = "{\"error\":\"mxl_domain_id must be a UUID\"}";
                            return;
                        }
                    }
                    if (auto const* flow = leg.find("mxl_flow_id"))
                    {
                        channel->staged.flowId = flow->isNull() ? std::string() : flow->text();
                        if (!channel->staged.flowId.empty() && !isUuid(channel->staged.flowId))
                        {
                            res.status = 400;
                            res.body = "{\"error\":\"mxl_flow_id must be a UUID\"}";
                            return;
                        }
                    }
                }
                bool immediate = false;
                if (auto const* activation = body.find("activation"); activation && activation->isObject())
                {
                    if (auto const* mode = activation->find("mode"); mode && mode->text() == "activate_immediate") immediate = true;
                }
                if (immediate) activate(*channel);
                logInfo("nmos receiver " + channel->receiverId + (immediate ? " activated" : " staged"));
            }
            text(connectionJson(false, *channel, channel->staged, v, true));
            return;
        }
    }
    if (auto* channel = match(sendPrefix, true))
    {
        auto rest = path.substr(sendPrefix.size() + channel->senderId.size());
        if (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
        if (rest.empty())
        {
            text(std::string("{\"id\":\"") + channel->senderId + "\",\"device_id\":\"" + deviceId_ +
                 "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"interface_bindings\":[]}");
            return;
        }
        if (rest == "constraints")
        {
            text("[{\"mxl_domain_id\":{\"enum\":[\"" + config_.outputDomainId + "\"]},\"mxl_flow_id\":{\"enum\":[\"" + channel->flowId + "\"]}}]");
            return;
        }
        if (rest == "active" || rest == "staged")
        {
            text(connectionJson(true, *channel, channel->active, v, rest == "staged"));
            return;
        }
    }
    for (auto const& channel : channels_)
    {
        if (path == "/x-nmos/node/v1.3/receivers/" + channel.receiverId)
        {
            text(receiverJson(channel, v));
            return;
        }
        if (path == "/x-nmos/node/v1.3/senders/" + channel.senderId)
        {
            text(senderJson(channel, v));
            return;
        }
    }
    res.status = 404;
    res.body = "{\"error\":\"not found\"}";
}

} // namespace cc
