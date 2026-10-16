// Interlaced flows on a real MXL domain: what MXL makes of the flow_def the corrector writes for an
// interlaced input (fields, at twice the declared rate), and that grainFits refuses a field read as
// a frame (the 1.2.2 crash: a 1080i input was read and written as 1080 lines).

#include "doctest/doctest.h"

#include "v210.hpp"
#include "video_format.hpp"

#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace cc;

namespace
{
struct TempDomain
{
    std::filesystem::path root;
    TempDomain()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "cc-interlace-XXXXXX").string();
        char* made = ::mkdtemp(pattern.data());
        REQUIRE(made != nullptr);
        root = made;
        std::ofstream(root / "domain_def.json") << "{\"id\":\"5e0c0c0c-0000-4000-8000-0000000000d1\",\"label\":\"interlace test\",\"description\":\"\",\"tags\":{}}\n";
        std::ofstream(root / "options.json") << "{\"urn:x-mxl:option:history_duration/v1.0\": 200000000}\n";
    }
    ~TempDomain()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
} // namespace

TEST_CASE("MXL stores the corrector's interlaced output flow as fields at the field rate")
{
    for (auto const* mode : {"interlaced_tff", "interlaced_bff"})
    {
        TempDomain domain;
        auto* instance = mxlCreateInstance(domain.root.c_str(), nullptr);
        REQUIRE(instance != nullptr);
        VideoFormat format;
        format.width = 1920;
        format.height = 1080;
        format.rateNum = 25;
        format.rateDen = 1;
        format.interlace = mode;
        auto const geo = grainGeometry(format);
        auto const json = flowJson("5e0c0c0c-0000-4000-8000-000000000010", "CC 1 Video", "CC 1:Video", format, "5e0c0c0c-0000-4000-8000-000000000011",
            "5e0c0c0c-0000-4000-8000-000000000012");
        mxlFlowWriter writer = nullptr;
        mxlFlowConfigInfo config{};
        bool created = false;
        REQUIRE(mxlCreateFlowWriter(instance, json.c_str(), nullptr, &writer, &config, &created) == MXL_STATUS_OK);
        CHECK(created);
        CHECK(config.common.grainRate.numerator == geo.rateNum);
        CHECK(config.common.grainRate.denominator == geo.rateDen);
        CHECK(config.discrete.sliceSizes[0] == static_cast<std::uint32_t>(geo.lineBytes));

        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        REQUIRE(mxlFlowWriterOpenGrain(writer, 1000, &info, &payload) == MXL_STATUS_OK);
        CHECK(info.grainSize == geo.bytes);
        CHECK(info.totalSlices == 540);
        CHECK(grainFits(geo, info.grainSize, info.totalSlices));
        // The same grain read as 1080 progressive lines (what 1.2.2 did) does not fit.
        VideoFormat asFrame = format;
        asFrame.interlace = "progressive";
        CHECK_FALSE(grainFits(grainGeometry(asFrame), info.grainSize, info.totalSlices));
        info.validSlices = info.totalSlices;
        CHECK(mxlFlowWriterCommitGrain(writer, &info) == MXL_STATUS_OK);

        // A reader sees the same field geometry and the field rate.
        mxlFlowReader reader = nullptr;
        REQUIRE(mxlCreateFlowReader(instance, "5e0c0c0c-0000-4000-8000-000000000010", nullptr, &reader) == MXL_STATUS_OK);
        mxlFlowInfo flowInfo{};
        REQUIRE(mxlFlowReaderGetInfo(reader, &flowInfo) == MXL_STATUS_OK);
        CHECK(flowInfo.config.common.grainRate.numerator == 50);
        mxlGrainInfo read{};
        std::uint8_t* data = nullptr;
        REQUIRE(mxlFlowReaderGetGrainNonBlocking(reader, 1000, &read, &data) == MXL_STATUS_OK);
        CHECK(grainFits(geo, read.grainSize, read.totalSlices));
        mxlReleaseFlowReader(instance, reader);
        mxlReleaseFlowWriter(instance, writer);
        mxlDestroyInstance(instance);
    }
}
