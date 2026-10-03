#include "api.hpp"
#include "http.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "doctest/doctest.h"

using namespace cc;

namespace
{
std::string httpGet(int port, std::string const& method, std::string const& path, std::string const& body = {})
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return {};
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
                      body;
    ::send(fd, req.data(), req.size(), 0);
    std::string out;
    char buf[4096];
    while (true)
    {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
    }
    ::close(fd);
    return out;
}
} // namespace

TEST_CASE("http api validates controls and serves health")
{
    auto dir = std::filesystem::temp_directory_path() / "mxl-cc-api-test";
    std::filesystem::remove_all(dir);
    auto cfg = loadConfig({{"CC_CHANNELS", "1"}, {"CC_STATE_DIR", dir.string()}, {"NMOS_SEED", "api-test"}}, {});
    ControlStore store(cfg);
    RuntimeBoard runtime(1);
    NmosNode nmos(cfg);
    std::string ui = "<!doctype html><title>cc</title>";
    Services services{&store, &runtime, &nmos, &ui, true};
    HttpServer server([&](HttpRequest const& req, HttpResponse& res) { dispatchHttp(services, req, res); });
    std::string error;
    REQUIRE(server.start(0, error));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto live = httpGet(server.port(), "GET", "/livez");
    CHECK(live.find("200") != std::string::npos);
    CHECK(live.find("ok") != std::string::npos);
    auto bad = httpGet(server.port(), "PATCH", "/api/v1/channels/1/controls", "{\"gain\":400}");
    CHECK(bad.find("400") != std::string::npos);
    auto ok = httpGet(server.port(), "PATCH", "/api/v1/channels/1/controls", "{\"gain\":50,\"saturation\":0}");
    CHECK(ok.find("200") != std::string::npos);
    CHECK(store.live(1).gain == 50);
    CHECK(store.live(1).saturation == 0);
    auto ready = httpGet(server.port(), "GET", "/readyz");
    CHECK(ready.find("200") != std::string::npos);
    auto exported = httpGet(server.port(), "GET", "/api/v1/config/export");
    CHECK(exported.find("\"version\":") != std::string::npos);
    CHECK(exported.find("\"routes\"") != std::string::npos);
    auto imported = httpGet(server.port(), "POST", "/api/v1/config/import",
        "{\"channels\":[{\"active\":\"a\",\"a\":{\"gain\":70},\"b\":{\"gain\":100}}],\"presets\":[],\"routes\":[{\"channel\":1,\"master_enable\":true,\"mxl_domain_id\":\"11111111-1111-4111-8111-111111111111\",\"mxl_flow_id\":\"22222222-2222-4222-8222-222222222222\"}]}");
    CHECK(imported.find("200") != std::string::npos);
    CHECK(store.live(1).gain == 70);
    CHECK(nmos.route(1).enable);
    CHECK(nmos.route(1).flowId == "22222222-2222-4222-8222-222222222222");
    auto page = httpGet(server.port(), "GET", "/");
    CHECK(page.find("text/html") != std::string::npos);
    auto metrics = httpGet(server.port(), "GET", "/metrics");
    CHECK(metrics.find("mxl_color_corrector_channel_state") != std::string::npos);
    auto nmosBody = httpGet(server.port(), "GET", "/x-nmos/node/v1.3/self");
    CHECK(nmosBody.find(nmos.nodeId()) != std::string::npos);
    auto staged = httpGet(server.port(), "PATCH", "/x-nmos/connection/v1.1/single/receivers/" + [&] {
        auto summary = nmos.summary();
        return summary.find("channels")->a[0].find("receiver_id")->text();
    }() + "/staged",
        "{\"master_enable\":true,\"sender_id\":null,\"activation\":{\"mode\":\"activate_immediate\"},\"transport_params\":[{\"mxl_domain_id\":\"11111111-1111-4111-8111-111111111111\",\"mxl_flow_id\":\"22222222-2222-4222-8222-222222222222\"}]}");
    CHECK(staged.find("200") != std::string::npos);
    auto route = nmos.route(1);
    CHECK(route.enable);
    CHECK(route.domainId == "11111111-1111-4111-8111-111111111111");
    CHECK(route.flowId == "22222222-2222-4222-8222-222222222222");
    server.stop();
    std::filesystem::remove_all(dir);
}
