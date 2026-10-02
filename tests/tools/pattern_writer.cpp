#include "color/bt709.hpp"
#include "v210.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
std::string arg(int argc, char** argv, std::string const& name, std::string const& fallback)
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

int argInt(int argc, char** argv, std::string const& name, int fallback)
{
    auto text = arg(argc, argv, name, "");
    return text.empty() ? fallback : std::stoi(text);
}

void putPixel(std::vector<std::uint16_t>& y, std::vector<std::uint16_t>& cb, std::vector<std::uint16_t>& cr, int x, double R, double G, double B)
{
    double Y, Cb, Cr;
    cc::bt709::rgbToCode(R, G, B, Y, Cb, Cr);
    y[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(std::lround(std::clamp(Y, 0.0, 1023.0)));
    if ((x % 2) == 0)
    {
        cb[static_cast<std::size_t>(x / 2)] = static_cast<std::uint16_t>(std::lround(std::clamp(Cb, 0.0, 1023.0)));
        cr[static_cast<std::size_t>(x / 2)] = static_cast<std::uint16_t>(std::lround(std::clamp(Cr, 0.0, 1023.0)));
    }
}
} // namespace

int main(int argc, char** argv)
{
    auto domain = arg(argc, argv, "--domain", "/tmp/mxl-cc-src");
    auto domainId = arg(argc, argv, "--domain-id", "11111111-1111-4111-8111-111111111111");
    auto flowId = arg(argc, argv, "--flow-id", "22222222-2222-4222-8222-222222222222");
    int width = argInt(argc, argv, "--width", 1920);
    int height = argInt(argc, argv, "--height", 1080);
    int rate = argInt(argc, argv, "--rate", 50);
    int sliceLines = argInt(argc, argv, "--slices", 16);
    int sleepUs = argInt(argc, argv, "--sleep-us", 50);
    int seconds = argInt(argc, argv, "--seconds", 8);
    auto interlace = arg(argc, argv, "--interlace", "progressive");
    if (width % 2 || width < 2 || height < 1 || sliceLines < 1) return 2;

    std::filesystem::create_directories(domain);
    {
        std::ofstream out(std::filesystem::path(domain) / "domain_def.json");
        out << "{\"id\":\"" << domainId << "\",\"label\":\"cc-pattern\"}\n";
    }
    {
        std::ofstream out(std::filesystem::path(domain) / "options.json");
        out << "{\"urn:x-mxl:option:history_duration/v1.0\": 200000000}\n";
    }
    auto* instance = mxlCreateInstance(domain.c_str(), nullptr);
    if (!instance)
    {
        std::cerr << "mxlCreateInstance failed\n";
        return 1;
    }
    std::string json = std::string("{\n  \"id\": \"") + flowId + "\",\n  \"label\": \"CC bars\",\n  \"description\": \"bars and ramp\",\n"
        "  \"format\": \"urn:x-nmos:format:video\",\n  \"media_type\": \"video/v210\",\n"
        "  \"tags\": {\"urn:x-nmos:tag:grouphint/v1.0\": [\"Bars:Video\"]},\n  \"grain_rate\": {\"numerator\": " +
        std::to_string(rate) + ", \"denominator\": 1},\n  \"frame_width\": " + std::to_string(width) + ",\n  \"frame_height\": " + std::to_string(height) +
        ",\n  \"interlace_mode\": \"" + interlace + "\",\n  \"colorspace\": \"BT709\",\n  \"components\": [\n"
        "    {\"name\": \"Y\", \"width\": " + std::to_string(width) + ", \"height\": " + std::to_string(height) + ", \"bit_depth\": 10},\n"
        "    {\"name\": \"Cb\", \"width\": " + std::to_string(width / 2) + ", \"height\": " + std::to_string(height) + ", \"bit_depth\": 10},\n"
        "    {\"name\": \"Cr\", \"width\": " + std::to_string(width / 2) + ", \"height\": " + std::to_string(height) + ", \"bit_depth\": 10}\n  ]\n}\n";
    mxlFlowWriter writer = nullptr;
    bool created = false;
    if (mxlCreateFlowWriter(instance, json.c_str(), nullptr, &writer, nullptr, &created) != MXL_STATUS_OK)
    {
        std::cerr << "mxlCreateFlowWriter failed\n";
        return 1;
    }
    int const stride = cc::v210Stride(width);
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(stride * height));
    double const bars[8][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}};
    int const barW = std::max(1, width / 8);
    int const split = height * 2 / 3;
    for (int row = 0; row < height; ++row)
    {
        std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
        std::vector<std::uint16_t> cb(static_cast<std::size_t>(width / 2), 512);
        std::vector<std::uint16_t> cr(static_cast<std::size_t>(width / 2), 512);
        for (int x = 0; x < width; ++x)
        {
            if (row < split)
            {
                int const band = std::min(7, x / barW);
                putPixel(y, cb, cr, x, bars[band][0], bars[band][1], bars[band][2]);
            }
            else
            {
                double const t = width <= 1 ? 0 : static_cast<double>(x) / static_cast<double>(width - 1);
                putPixel(y, cb, cr, x, t, t, t);
            }
        }
        cc::packV210Line(frame.data() + static_cast<std::size_t>(row * stride), width, stride, y.data(), cb.data(), cr.data());
    }
    mxlRational edit{rate, 1};
    auto index = mxlGetCurrentIndex(&edit);
    auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::cerr << "{\"flow\":\"" << flowId << "\",\"domain\":\"" << domainId << "\",\"index\":" << index << "}\n";
    while (std::chrono::steady_clock::now() < until)
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowWriterOpenGrain(writer, index, &info, &payload) != MXL_STATUS_OK || payload == nullptr) break;
        int written = 0;
        while (written < height)
        {
            int const n = std::min(sliceLines, height - written);
            std::memcpy(payload + static_cast<std::size_t>(written) * static_cast<std::size_t>(stride),
                frame.data() + static_cast<std::size_t>(written) * static_cast<std::size_t>(stride), static_cast<std::size_t>(n) * static_cast<std::size_t>(stride));
            written += n;
            info.index = index;
            info.flags = 0;
            info.validSlices = static_cast<std::uint16_t>(std::min(written, height));
            if (mxlFlowWriterCommitGrain(writer, &info) != MXL_STATUS_OK) break;
            if (sleepUs > 0 && written < height) ::usleep(static_cast<useconds_t>(sleepUs));
        }
        ++index;
    }
    mxlReleaseFlowWriter(instance, writer);
    mxlDestroyInstance(instance);
    return 0;
}
