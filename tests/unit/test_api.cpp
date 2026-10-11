#include "api.hpp"
#include "http.hpp"
#include "version.hpp"

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
std::string httpGet(int port, std::string const& method, std::string const& path, std::string const& body = {}, std::string const& headers = {})
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
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" + headers + "Content-Length: " + std::to_string(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body;
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

// "200" from "HTTP/1.1 200 OK".
std::string statusOf(std::string const& response)
{
    return response.size() > 12 ? response.substr(9, 3) : std::string();
}

bool hasHeader(std::string const& response, std::string const& line)
{
    return response.substr(0, response.find("\r\n\r\n") + 2).find("\r\n" + line) != std::string::npos;
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
    auto devices = httpGet(server.port(), "GET", "/x-nmos/node/v1.3/devices");
    CHECK(devices.find("{\"href\":\"http://" + cfg.nmosHostAddress + ":" + std::to_string(cfg.nmosPort) + "/x-nmos/connection/v1.1/\",\"type\":\"urn:x-nmos:control:sr-ctrl/v1.1\"}") != std::string::npos);
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
    auto receiverId = nmos.summary().find("channels")->a[0].find("receiver_id")->text();
    auto off = httpGet(server.port(), "PATCH", "/x-nmos/connection/v1.1/single/receivers/" + receiverId + "/staged",
        "{\"master_enable\":false,\"activation\":{\"mode\":\"activate_immediate\"}}");
    CHECK(off.find("200") != std::string::npos);
    CHECK_FALSE(nmos.route(1).enable);
    server.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("widgets: the list with CORS for listed origins, the pages with CSP frame-ancestors")
{
    auto dir = std::filesystem::temp_directory_path() / "mxl-cc-widget-test";
    std::filesystem::remove_all(dir);
    auto cfg = loadConfig({{"CC_CHANNELS", "2"}, {"CC_STATE_DIR", dir.string()}, {"NMOS_SEED", "widget-test"},
                              {"WIDGET_FRAME_ANCESTORS", "'self' https://designer.example"}},
        {});
    ControlStore store(cfg);
    RuntimeBoard runtime(2);
    NmosNode nmos(cfg);
    std::string ui = "<!doctype html><title>cc</title>";
    Services services{&store, &runtime, &nmos, &ui, true};
    HttpServer server([&](HttpRequest const& req, HttpResponse& res) { dispatchHttp(services, req, res); });
    std::string error;
    REQUIRE(server.start(0, error));
    int const port = server.port();

    auto const list = httpGet(port, "GET", "/widgets", {}, "Origin: https://designer.example\r\n");
    CHECK(statusOf(list) == "200");
    CHECK(hasHeader(list, "Access-Control-Allow-Origin: https://designer.example\r\n"));
    CHECK(hasHeader(list, "Vary: Origin\r\n"));
    auto const body = parseJson(list.substr(list.find("\r\n\r\n") + 4));
    REQUIRE(body.isArray());
    REQUIRE(body.a.size() == 2);
    CHECK(body.a[0].find("id")->text() == "controls");
    CHECK(body.a[0].find("min_size")->find("w")->num() == 480);
    CHECK(body.a[0].find("min_size")->find("h")->num() == 360);
    CHECK(body.a[0].find("params")->find("properties")->find("channel")->find("maximum")->num() == 2);
    CHECK(body.a[0].find("params")->find("required")->a[0].text() == "channel");
    CHECK(body.a[0].find("version")->text() == kVersion);
    CHECK(body.a[1].find("id")->text() == "bypass");
    auto const evil = httpGet(port, "GET", "/widgets", {}, "Origin: https://evil.example\r\n");
    CHECK(statusOf(evil) == "200");
    CHECK(evil.find("Access-Control-Allow-Origin") == std::string::npos);
    CHECK(httpGet(port, "GET", "/widgets").find("Access-Control-Allow-Origin") == std::string::npos);
    CHECK(statusOf(httpGet(port, "POST", "/widgets", "{}")) == "405");
    CHECK(statusOf(httpGet(port, "OPTIONS", "/widgets")) == "405");

    auto const page = httpGet(port, "GET", "/widget/controls?channel=2&theme=transparent");
    CHECK(statusOf(page) == "200");
    CHECK(hasHeader(page, "Content-Type: text/html"));
    CHECK(hasHeader(page, "Content-Security-Policy: frame-ancestors 'self' https://designer.example\r\n"));
    CHECK(page.find("X-Frame-Options") == std::string::npos);
    CHECK(page.find("Access-Control-Allow-Origin") == std::string::npos);
    CHECK(page.find("<title>cc</title>") != std::string::npos);
    CHECK(statusOf(httpGet(port, "GET", "/widget/bypass?channel=1&theme=light")) == "200");
    CHECK(statusOf(httpGet(port, "GET", "/widget/controls")) == "400");
    CHECK(statusOf(httpGet(port, "GET", "/widget/controls?channel=0")) == "400");
    CHECK(statusOf(httpGet(port, "GET", "/widget/controls?channel=3")) == "400");
    CHECK(statusOf(httpGet(port, "GET", "/widget/controls?channel=x")) == "400");
    CHECK(statusOf(httpGet(port, "GET", "/widget/bypass?channel=1&theme=pink")) == "400");
    CHECK(statusOf(httpGet(port, "GET", "/widget/tally?channel=1")) == "404");
    // The rest of the API keeps its CORS.
    CHECK(hasHeader(httpGet(port, "GET", "/livez"), "Access-Control-Allow-Origin: *\r\n"));
    server.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("a WebSocket client that vanishes does not break the server")
{
    // 1.0.5: broadcast() closed a dead client's socket while its thread kept reading the number, so a new
    // HTTP connection on that number lost its request to the thread (empty answers, a spinning thread).
    HttpServer server([](HttpRequest const& req, HttpResponse& res) {
        res.contentType = "text/plain";
        res.body = "ok\n";
    });
    server.setWebSocket("/api/v1/events", [](std::string const&) {}, [] { return std::string("{\"type\":\"state\"}"); });
    std::string error;
    REQUIRE(server.start(0, error));
    for (int client = 0; client < 4; ++client)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(server.port()));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        std::string const upgrade = "GET /api/v1/events HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        ::send(fd, upgrade.data(), upgrade.size(), 0);
        char buf[512];
        CHECK(::recv(fd, buf, sizeof(buf), 0) > 0);
        linger const reset{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        ::close(fd); // gone without a close frame
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int answered = 0;
    for (int i = 0; i < 40; ++i)
    {
        server.broadcast(std::string(2000, 'x'));
        answered += statusOf(httpGet(server.port(), "GET", "/livez")) == "200" ? 1 : 0;
    }
    CHECK(answered == 40);
    server.stop();
}

TEST_CASE("http api: widened ranges, setting origins, status version, preset names with spaces")
{
    auto dir = std::filesystem::temp_directory_path() / "mxl-cc-range-test";
    std::filesystem::remove_all(dir);
    auto cfg = loadConfig({{"CC_CHANNELS", "1"}, {"CC_STATE_DIR", dir.string()}, {"NMOS_SEED", "range-test"}}, {});
    ControlStore store(cfg);
    RuntimeBoard runtime(1);
    NmosNode nmos(cfg);
    Services services{&store, &runtime, &nmos, nullptr, true};
    HttpServer server([&](HttpRequest const& req, HttpResponse& res) { dispatchHttp(services, req, res); });
    std::string error;
    REQUIRE(server.start(0, error));
    int const port = server.port();
    auto const patch = [&](std::string const& body) { return statusOf(httpGet(port, "PATCH", "/api/v1/channels/1/controls", body)); };

    CHECK(patch(R"({"pedestal":100,"brightness":-100})") == "200");
    CHECK(store.live(1).pedestal == 100);
    CHECK(store.live(1).brightness == -100);
    CHECK(patch(R"({"black":{"r":-100},"white":{"g":100}})") == "200");
    CHECK(store.live(1).black.r == -100);
    CHECK(store.live(1).white.g == 100);
    CHECK(patch(R"({"pedestal":-100.5})") == "400");
    CHECK(patch(R"({"brightness":101})") == "400");
    CHECK(patch(R"({"white":{"b":-101}})") == "400");
    CHECK(patch(R"({"black":{"g":100.01}})") == "400");
    CHECK(store.live(1).pedestal == 100);

    auto const config = httpGet(port, "GET", "/api/v1/config");
    CHECK(statusOf(config) == "200");
    CHECK(config.find(R"({"key":"CC_CHANNELS","value":"1","source":"environment"})") != std::string::npos);
    CHECK(config.find(R"({"key":"CC_CLIP","value":"legal","source":"default"})") != std::string::npos);
    CHECK(config.find(R"({"key":"WIDGET_FRAME_ANCESTORS","value":"'self'","source":"default"})") != std::string::npos);
    auto const status = httpGet(port, "GET", "/api/v1/status");
    CHECK(status.find(std::string(R"("version":")") + kVersion + "\"") != std::string::npos);
    CHECK(status.find(R"("mxl_revision":")") != std::string::npos);

    CHECK(statusOf(httpGet(port, "POST", "/api/v1/channels/1/presets", R"({"name":"warm look"})")) == "200");
    CHECK(patch(R"({"pedestal":0})") == "200");
    CHECK(statusOf(httpGet(port, "POST", "/api/v1/channels/1/presets/warm%20look/recall")) == "200");
    CHECK(store.live(1).pedestal == 100);
    CHECK(statusOf(httpGet(port, "DELETE", "/api/v1/channels/1/presets/warm%20look")) == "200");
    server.stop();
    std::filesystem::remove_all(dir);
}
