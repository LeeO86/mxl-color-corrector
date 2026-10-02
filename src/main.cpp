#include "api.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "nmos.hpp"
#include "store.hpp"
#include "util/log.hpp"
#include "version.hpp"

#include "webui.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <thread>

namespace
{
std::atomic<int> gSignal{0};

void onSignal(int signal)
{
    gSignal.store(signal);
}

std::map<std::string, std::string> environment()
{
    std::map<std::string, std::string> env;
    for (char** item = environ; item && *item; ++item)
    {
        std::string text(*item);
        auto eq = text.find('=');
        if (eq == std::string::npos) continue;
        env.emplace(text.substr(0, eq), text.substr(eq + 1));
    }
    return env;
}
} // namespace

extern char** environ;

int main()
{
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    cc::Config config;
    try
    {
        config = cc::loadConfigFromEnv(environment());
    }
    catch (cc::ConfigError const& ex)
    {
        std::cerr << "{\"level\":\"error\",\"msg\":\"" << ex.what() << "\"}\n";
        return 78;
    }
    cc::setLogLevel(config.logLevel);
    cc::logInfo(std::string("mxl-color-corrector ") + cc::kVersion + " channels " + std::to_string(config.channels));
    cc::ControlStore store(config);
    cc::RuntimeBoard runtime(config.channels);
    cc::NmosNode nmos(config);
    std::string ui(cc::webui::indexHtml());
    cc::Services services{&store, &runtime, &nmos, &ui, config.webPort == config.nmosPort};
    cc::HttpServer web([&](cc::HttpRequest const& req, cc::HttpResponse& res) { cc::dispatchHttp(services, req, res); });
    web.setWebSocket(
        "/api/v1/events", [&](std::string const& message) { cc::handleEvent(services, message); }, [&] { return cc::eventsHello(services); });
    store.setListener([&](int) { web.broadcast(cc::eventsHello(services)); });
    std::string error;
    if (!web.start(config.webPort, error))
    {
        cc::logError(error);
        return 75;
    }
    std::unique_ptr<cc::HttpServer> nmosHttp;
    if (config.nmosPort != config.webPort && config.nmosPort != 0)
    {
        nmosHttp = std::make_unique<cc::HttpServer>([&](cc::HttpRequest const& req, cc::HttpResponse& res) { nmos.handle(req, res); });
        if (!nmosHttp->start(config.nmosPort, error))
        {
            cc::logError(error);
            return 75;
        }
    }
    try
    {
        cc::Engine engine(config, store, runtime, nmos);
        nmos.start();
        engine.start();
        cc::logInfo("web port " + std::to_string(web.port()) + " nmos port " + std::to_string(config.nmosPort));
        while (gSignal.load() == 0)
        {
            web.broadcast(cc::eventsHello(services));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        engine.stop();
    }
    catch (std::exception const& ex)
    {
        cc::logError(ex.what());
        nmos.stop();
        return 75;
    }
    nmos.stop();
    web.stop();
    if (nmosHttp) nmosHttp->stop();
    return gSignal.load() == SIGTERM ? 143 : 0;
}
