// Interlaced flows (video_format.hpp): MXL stores them as fields. The corrector reads a field and
// writes a field; a grain that does not match the declared format is never read past its end.

#include "doctest/doctest.h"

#include "api.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "mode.hpp"
#include "nmos.hpp"
#include "preview.hpp"
#include "store.hpp"
#include "v210.hpp"
#include "video_format.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace cc;

namespace
{
VideoFormat format1080i(std::string const& mode, int rateNum = 25, int rateDen = 1)
{
    VideoFormat format;
    format.width = 1920;
    format.height = 1080;
    format.rateNum = rateNum;
    format.rateDen = rateDen;
    format.interlace = mode;
    return format;
}

// The height in a baseline JPEG's SOF0 segment.
int jpegHeight(std::vector<std::uint8_t> const& jpeg)
{
    for (std::size_t i = 0; i + 8 < jpeg.size(); ++i)
    {
        if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xC0) return (jpeg[i + 5] << 8) | jpeg[i + 6];
    }
    return -1;
}

std::string request(int port, std::string const& path)
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
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
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

TEST_CASE("1080i50: a grain is one field of 540 lines at 50 fields per second")
{
    for (auto const* mode : {"interlaced_tff", "interlaced_bff"})
    {
        auto const geo = grainGeometry(format1080i(mode));
        CHECK(geo.interlaced);
        CHECK(geo.lines == 540);
        CHECK(geo.lineBytes == v210Stride(1920));
        CHECK(geo.bytes == static_cast<std::size_t>(v210Stride(1920)) * 540);
        CHECK(geo.rateNum == 50);
        CHECK(geo.rateDen == 1);
    }
    auto const ntsc = grainGeometry(format1080i("interlaced_tff", 30000, 1001));
    CHECK(ntsc.rateNum == 60000);
    CHECK(ntsc.rateDen == 1001);
    auto const progressive = grainGeometry(format1080i("progressive", 50, 1));
    CHECK_FALSE(progressive.interlaced);
    CHECK(progressive.lines == 1080);
    CHECK(progressive.rateNum == 50);
    // MXL refuses interlaced_psf, so it is never a field layout here.
    CHECK_FALSE(isInterlaced("interlaced_psf"));
    CHECK_FALSE(isInterlaced("progressive"));
}

TEST_CASE("grainFits: a smaller grain or another slice count is refused, never read past its end")
{
    auto const field = grainGeometry(format1080i("interlaced_tff"));
    auto const frame = grainGeometry(format1080i("progressive", 50, 1));
    auto const fieldBytes = static_cast<std::uint32_t>(field.bytes);
    CHECK(grainFits(field, fieldBytes, 540));
    // 1.2.2 read a whole frame from a field grain: the field's grain is half the frame's.
    CHECK_FALSE(grainFits(frame, fieldBytes, 540));
    // The other way round (a frame grain read as a field) has the wrong slice count.
    CHECK_FALSE(grainFits(field, static_cast<std::uint32_t>(frame.bytes), 1080));
    CHECK_FALSE(grainFits(field, fieldBytes - 1, 540));
    CHECK_FALSE(grainFits(field, 0, 0));
    CHECK_FALSE(grainFits(GrainGeometry{}, fieldBytes, 540));
}

TEST_CASE("the output flow_def mirrors the input's interlace mode and declared rate")
{
    for (auto const* mode : {"interlaced_tff", "interlaced_bff", "progressive"})
    {
        auto const in = format1080i(mode);
        auto const json = flowJson("5e0c0c0c-0000-4000-8000-000000000002", "CC 1 Video", "CC 1:Video", in, "s", "d");
        VideoFormat out;
        REQUIRE(parseFlow(json, out));
        CHECK(out.interlace == mode);
        // IS-04 and MXL: grain_rate of an interlaced flow is its frame rate; MXL doubles it.
        CHECK(out.rateNum == 25);
        CHECK(out.rateDen == 1);
        CHECK(out.height == 1080);
        CHECK(grainGeometry(out).lines == grainGeometry(in).lines);
    }
    VideoFormat odd;
    CHECK_FALSE(parseFlow("{\"frame_width\":1920,\"frame_height\":1081,\"interlace_mode\":\"interlaced_tff\",\"media_type\":\"video/v210\","
                          "\"grain_rate\":{\"numerator\":25,\"denominator\":1}}",
        odd));
}

TEST_CASE("interlaced input stays whole-grain; the decision uses the field's lines")
{
    int const line = v210Stride(1920);
    auto const decision = decideMode(true, true, 540, 540, static_cast<std::uint32_t>(line), line);
    CHECK_FALSE(decision.slice);
    CHECK(decision.reason == FallbackReason::Interlaced);
}

TEST_CASE("the preview of a field keeps the frame's aspect")
{
    int const stride = v210Stride(64);
    std::vector<std::uint8_t> field(static_cast<std::size_t>(stride) * 18, 0);
    auto const squashed = encodePreviewJpeg(field.data(), 64, 18, stride, 64);
    auto const shown = encodePreviewJpeg(field.data(), 64, 18, stride, 64, 36);
    CHECK(jpegHeight(squashed) == 18);
    CHECK(jpegHeight(shown) == 36);
}

TEST_CASE("NMOS: the output flow advertises its interlace mode; the receiver takes tff and bff at 25 and 30000/1001")
{
    auto dir = std::filesystem::temp_directory_path() / "mxl-cc-interlace-test";
    std::filesystem::remove_all(dir);
    auto cfg = loadConfig({{"CC_CHANNELS", "1"}, {"CC_STATE_DIR", dir.string()}, {"NMOS_SEED", "interlace-test"}}, {});
    ControlStore store(cfg);
    RuntimeBoard runtime(1);
    NmosNode nmos(cfg);
    OutputFormat published;
    published.flowId = "5e0c0c0c-0000-4000-8000-000000000003";
    published.rateNum = 25;
    published.interlaced = true;
    published.interlaceMode = "interlaced_bff";
    nmos.setOutput(1, published);
    std::string ui = "<!doctype html><title>cc</title>";
    Services services{&store, &runtime, &nmos, &ui, true};
    HttpServer server([&](HttpRequest const& req, HttpResponse& res) { dispatchHttp(services, req, res); });
    std::string error;
    REQUIRE(server.start(0, error));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto flows = request(server.port(), "/x-nmos/node/v1.3/flows");
    CHECK(flows.find("\"interlace_mode\":\"interlaced_bff\"") != std::string::npos);
    CHECK(flows.find("\"grain_rate\":{\"numerator\":25,\"denominator\":1}") != std::string::npos);
    auto receivers = request(server.port(), "/x-nmos/node/v1.3/receivers");
    CHECK(receivers.find("\"urn:x-nmos:cap:format:interlace_mode\":{\"enum\":[\"interlaced_tff\",\"interlaced_bff\"]}") != std::string::npos);
    CHECK(receivers.find("interlaced_psf") == std::string::npos);
    // The unsupported state and its counter.
    runtime.setState(1, 5, 0, 1);
    runtime.addUnsupported(1);
    auto metrics = request(server.port(), "/metrics");
    CHECK(metrics.find("mxl_color_corrector_channel_state{channel=\"1\",state=\"unsupported\"} 1") != std::string::npos);
    CHECK(metrics.find("mxl_color_corrector_unsupported_grains_total{channel=\"1\"} 1") != std::string::npos);
    auto status = request(server.port(), "/api/v1/status");
    CHECK(status.find("\"state\":\"unsupported\"") != std::string::npos);
    CHECK(status.find("\"unsupported_grains\":1") != std::string::npos);
    server.stop();
    std::filesystem::remove_all(dir);
}
