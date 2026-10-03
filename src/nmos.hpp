#pragma once

#include "config.hpp"
#include "http.hpp"
#include "util/json.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cc
{

struct NmosRoute
{
    bool enable = false;
    std::string domainId;
    std::string flowId;
    std::string senderId;
};

struct OutputFormat
{
    std::string flowId;
    int width = 1920;
    int height = 1080;
    int rateNum = 50;
    int rateDen = 1;
    bool interlaced = false;
    std::string colorspace = "BT709";
};

class NmosNode
{
public:
    explicit NmosNode(Config config);
    ~NmosNode();

    void start();
    void stop();
    void handle(HttpRequest const& req, HttpResponse& res);
    [[nodiscard]] NmosRoute route(int channel) const;
    void setOutput(int channel, OutputFormat format);
    [[nodiscard]] OutputFormat output(int channel) const;
    [[nodiscard]] Json summary() const;
    [[nodiscard]] std::string nodeId() const { return nodeId_; }
    [[nodiscard]] std::string deviceId() const { return deviceId_; }
    [[nodiscard]] bool registered() const { return registered_.load(); }
    [[nodiscard]] bool ready() const;
    [[nodiscard]] Json exportRoutes() const;
    bool importRoutes(Json const& routes, std::string& error);

private:
    struct Leg
    {
        bool master = false;
        std::string domainId;
        std::string flowId;
        std::string senderId;
    };
    struct Channel
    {
        std::string receiverId;
        std::string sourceId;
        std::string senderId;
        std::string flowId;
        Leg staged;
        Leg active;
        OutputFormat format;
    };

    [[nodiscard]] std::string version() const;
    [[nodiscard]] std::string receiverJson(Channel const& channel, std::string const& version) const;
    [[nodiscard]] std::string senderJson(Channel const& channel, std::string const& version) const;
    [[nodiscard]] std::string connectionJson(bool sender, Channel const& channel, Leg const& leg, std::string const& version, bool staged) const;
    void activate(Channel& channel);
    void registryLoop();
    bool postRegistry(std::string const& body) const;
    void deregister() const;
    void loadRoutes();
    void persistRoutesUnlocked() const;
    [[nodiscard]] std::string origin() const;
    [[nodiscard]] std::string deviceLabel() const;
    [[nodiscard]] std::string tagsJson() const;

    Config config_;
    std::string nodeId_;
    std::string deviceId_;
    std::vector<Channel> channels_;
    mutable std::mutex mu_;
    std::thread registry_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> registered_{false};
};

} // namespace cc
