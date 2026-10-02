#include "v210.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::string arg(int argc, char** argv, std::string const& name, std::string const& fallback)
{
    for (int i = 1; i < argc - 1; ++i)
        if (argv[i] == name) return argv[i + 1];
    return fallback;
}
} // namespace

int main(int argc, char** argv)
{
    auto domain = arg(argc, argv, "--domain", "");
    auto flow = arg(argc, argv, "--flow", "");
    int width = std::stoi(arg(argc, argv, "--width", "1920"));
    int height = std::stoi(arg(argc, argv, "--height", "1080"));
    int timeoutMs = std::stoi(arg(argc, argv, "--timeout-ms", "2000"));
    auto points = arg(argc, argv, "--points", "120,200;1200,200;1800,200;100,900");
    if (domain.empty() || flow.empty())
    {
        std::cerr << "usage: mxl-cc-sample --domain DIR --flow UUID\n";
        return 2;
    }
    auto* instance = mxlCreateInstance(domain.c_str(), nullptr);
    if (!instance) return 1;
    mxlFlowReader reader = nullptr;
    if (mxlCreateFlowReader(instance, flow.c_str(), nullptr, &reader) != MXL_STATUS_OK)
    {
        std::cerr << "{\"ok\":false,\"error\":\"reader\"}\n";
        return 1;
    }
    mxlGrainInfo info{};
    std::uint8_t* payload = nullptr;
    mxlStatus status = MXL_ERR_TIMEOUT;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::uint64_t index = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        mxlFlowRuntimeInfo runtime{};
        if (mxlFlowReaderGetRuntimeInfo(reader, &runtime) == MXL_STATUS_OK && runtime.headIndex != MXL_UNDEFINED_INDEX)
        {
            index = runtime.headIndex;
            status = mxlFlowReaderGetGrain(reader, index, 20000000, &info, &payload);
            if (status == MXL_STATUS_OK && payload && (info.flags & MXL_GRAIN_FLAG_INVALID) == 0 && info.validSlices == info.totalSlices) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (status != MXL_STATUS_OK || payload == nullptr)
    {
        std::cerr << "{\"ok\":false,\"status\":" << static_cast<int>(status) << "}\n";
        return 1;
    }
    int const stride = cc::v210Stride(width);
    std::cout << "{\"ok\":true,\"index\":" << index << ",\"valid_slices\":" << info.validSlices << ",\"total_slices\":" << info.totalSlices << ",\"points\":[";
    bool first = true;
    std::stringstream ss(points);
    std::string item;
    while (std::getline(ss, item, ';'))
    {
        auto comma = item.find(',');
        if (comma == std::string::npos) continue;
        int x = std::stoi(item.substr(0, comma));
        int y = std::stoi(item.substr(comma + 1));
        if (x < 0 || y < 0 || x >= width || y >= height) continue;
        std::vector<std::uint16_t> lineY(static_cast<std::size_t>(width));
        std::vector<std::uint16_t> lineCb(static_cast<std::size_t>(width / 2));
        std::vector<std::uint16_t> lineCr(static_cast<std::size_t>(width / 2));
        cc::unpackV210Line(payload + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride), width, lineY.data(), lineCb.data(), lineCr.data());
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"x\":" << x << ",\"y\":" << y << ",\"y_code\":" << lineY[static_cast<std::size_t>(x)]
                  << ",\"cb\":" << lineCb[static_cast<std::size_t>(x / 2)] << ",\"cr\":" << lineCr[static_cast<std::size_t>(x / 2)] << "}";
    }
    std::cout << "]}\n";
    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);
    (void)height;
    return 0;
}
