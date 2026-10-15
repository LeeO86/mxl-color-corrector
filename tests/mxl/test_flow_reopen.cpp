// A source flow deleted and re-created under the same id, on a real MXL domain in a temporary
// directory: the stale reader reports it, a re-open reads the new flow.

#include "doctest/doctest.h"

#include "mxl_source.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>

namespace
{
constexpr char kFlow[] = "5e0c0c0c-0000-4000-8000-000000000001";
constexpr int kWidth = 64;
constexpr int kHeight = 4;

struct TempDomain
{
    std::filesystem::path root;
    std::filesystem::path dir;

    TempDomain()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "cc-reopen-XXXXXX").string();
        char* made = ::mkdtemp(pattern.data());
        REQUIRE(made != nullptr);
        root = made;
        dir = root / "src";
        make();
    }
    ~TempDomain()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void make() const
    {
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "domain_def.json") << "{\"id\":\"5e0c0c0c-0000-4000-8000-0000000000d0\",\"label\":\"reopen test\",\"description\":\"\",\"tags\":{}}\n";
        std::ofstream(dir / "options.json") << "{\"urn:x-mxl:option:history_duration/v1.0\": 200000000}\n";
    }
};

std::string flowDef()
{
    auto const w = std::to_string(kWidth);
    auto const h = std::to_string(kHeight);
    return std::string("{\"id\":\"") + kFlow + "\",\"label\":\"reopen\",\"description\":\"reopen\",\"format\":\"urn:x-nmos:format:video\","
           "\"media_type\":\"video/v210\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"Reopen:Video\"]},\"grain_rate\":{\"numerator\":50,\"denominator\":1},\"frame_width\":" + w +
           ",\"frame_height\":" + h + ",\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"components\":["
           "{\"name\":\"Y\",\"width\":" + w + ",\"height\":" + h + ",\"bit_depth\":10},"
           "{\"name\":\"Cb\",\"width\":" + std::to_string(kWidth / 2) + ",\"height\":" + h + ",\"bit_depth\":10},"
           "{\"name\":\"Cr\",\"width\":" + std::to_string(kWidth / 2) + ",\"height\":" + h + ",\"bit_depth\":10}]}";
}

// A writer on an instance of its own, as another process would have.
struct Writer
{
    mxlInstance instance = nullptr;
    mxlFlowWriter writer = nullptr;

    explicit Writer(std::filesystem::path const& dir)
    {
        instance = mxlCreateInstance(dir.c_str(), nullptr);
        REQUIRE(instance != nullptr);
        bool created = false;
        REQUIRE(mxlCreateFlowWriter(instance, flowDef().c_str(), nullptr, &writer, nullptr, &created) == MXL_STATUS_OK);
        REQUIRE(created);
    }
    ~Writer() { stop(); }
    void write(std::uint64_t index, std::uint8_t fill) const
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        REQUIRE(mxlFlowWriterOpenGrain(writer, index, &info, &payload) == MXL_STATUS_OK);
        std::memset(payload, fill, info.grainSize);
        info.index = index;
        info.flags = 0;
        info.validSlices = info.totalSlices;
        REQUIRE(mxlFlowWriterCommitGrain(writer, &info) == MXL_STATUS_OK);
    }
    // The last writer's release deletes the flow, as a test player's graceful stop does.
    void stop()
    {
        if (writer) mxlReleaseFlowWriter(instance, writer);
        writer = nullptr;
        if (instance) mxlDestroyInstance(instance);
        instance = nullptr;
    }
};

std::uint8_t firstByte(mxlFlowReader reader, std::uint64_t index, mxlStatus* status = nullptr)
{
    mxlGrainInfo info{};
    std::uint8_t* payload = nullptr;
    auto const got = mxlFlowReaderGetGrain(reader, index, 20'000'000, &info, &payload);
    if (status) *status = got;
    return got == MXL_STATUS_OK && payload ? payload[0] : 0;
}

ino_t dataInode(std::filesystem::path const& dir)
{
    struct stat st{};
    return ::stat((dir / (std::string(kFlow) + ".mxl-flow") / "data").c_str(), &st) == 0 ? st.st_ino : 0;
}
} // namespace

TEST_CASE("a flow deleted and re-created under the same id is read again after a re-open")
{
    TempDomain domain;
    cc::SourceReader source;
    CHECK(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::FlowMissing);

    auto writer = std::make_unique<Writer>(domain.dir);
    for (std::uint64_t i = 100; i <= 104; ++i) writer->write(i, 0x11);
    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(firstByte(source.reader(), 104) == 0x11);
    CHECK(source.valid());

    // The writer stops: the flow is deleted. The old head stands; the reader is stale.
    writer->stop();
    CHECK_FALSE(std::filesystem::exists(domain.dir / (std::string(kFlow) + ".mxl-flow")));
    CHECK_FALSE(source.valid());
    mxlStatus status = MXL_STATUS_OK;
    firstByte(source.reader(), 105, &status);
    CHECK(status == MXL_ERR_FLOW_INVALID);
    CHECK(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::FlowMissing);
    CHECK(source.reader() == nullptr);

    // The writer is back under the same id: a new data file.
    writer = std::make_unique<Writer>(domain.dir);
    for (std::uint64_t i = 105; i <= 106; ++i) writer->write(i, 0x22);
    // (No reader holds the old file any more, so the file system may hand out its inode number again.)
    CHECK(dataInode(domain.dir) != 0);
    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(source.valid());
    CHECK(firstByte(source.reader(), 106) == 0x22);
    writer->write(107, 0x33);
    CHECK(firstByte(source.reader(), 107) == 0x33);
}

TEST_CASE("a flow re-created at once is caught by valid() while its old head stands")
{
    TempDomain domain;
    auto writer = std::make_unique<Writer>(domain.dir);
    writer->write(200, 0x11);
    cc::SourceReader source;
    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(firstByte(source.reader(), 200) == 0x11);

    // Stop and start within a moment, no read in between (the 0.6 ms re-create in the lab).
    writer->stop();
    writer = std::make_unique<Writer>(domain.dir);
    writer->write(201, 0x22);

    // The stale reader still shows the old head; it never reads past it unless asked.
    mxlFlowRuntimeInfo runtime{};
    REQUIRE(mxlFlowReaderGetRuntimeInfo(source.reader(), &runtime) == MXL_STATUS_OK);
    CHECK(runtime.headIndex == 200);
    CHECK_FALSE(source.valid());

    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    REQUIRE(mxlFlowReaderGetRuntimeInfo(source.reader(), &runtime) == MXL_STATUS_OK);
    CHECK(runtime.headIndex == 201);
    CHECK(firstByte(source.reader(), 201) == 0x22);
    CHECK(source.valid());
}

TEST_CASE("a flow re-created before its first grain is caught by the data file's inode")
{
    TempDomain domain;
    auto writer = std::make_unique<Writer>(domain.dir);
    cc::SourceReader source;
    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(source.valid());
    writer->stop();
    writer = std::make_unique<Writer>(domain.dir);
    CHECK_FALSE(source.valid());
    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(source.valid());
    writer->write(300, 0x44);
    CHECK(firstByte(source.reader(), 300) == 0x44);
}

TEST_CASE("a re-open gets a new reader while another channel still holds the stale one")
{
    TempDomain domain;
    auto writer = std::make_unique<Writer>(domain.dir);
    writer->write(400, 0x11);

    // MXL hands out one reader per flow id and instance while anyone holds it: on a shared
    // instance the second "open" is the first reader again, and stays stale after a re-create.
    auto* shared = mxlCreateInstance(domain.dir.c_str(), nullptr);
    REQUIRE(shared != nullptr);
    mxlFlowReader held = nullptr;
    mxlFlowReader again = nullptr;
    REQUIRE(mxlCreateFlowReader(shared, kFlow, nullptr, &held) == MXL_STATUS_OK);

    // Two channels on the same source, each with its instance.
    cc::SourceReader one;
    cc::SourceReader two;
    REQUIRE(one.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    REQUIRE(two.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);

    writer->stop();
    writer = std::make_unique<Writer>(domain.dir);
    writer->write(401, 0x22);

    REQUIRE(mxlCreateFlowReader(shared, kFlow, nullptr, &again) == MXL_STATUS_OK);
    CHECK(again == held);
    mxlStatus status = MXL_STATUS_OK;
    firstByte(again, 401, &status);
    CHECK(status == MXL_ERR_FLOW_INVALID);
    mxlReleaseFlowReader(shared, again);
    mxlReleaseFlowReader(shared, held);
    mxlDestroyInstance(shared);

    // Channel two keeps its stale reader; channel one re-opens and reads the new flow.
    CHECK_FALSE(two.valid());
    REQUIRE(one.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(one.valid());
    CHECK(firstByte(one.reader(), 401) == 0x22);
    CHECK_FALSE(two.valid());
    REQUIRE(two.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(firstByte(two.reader(), 401) == 0x22);
}

TEST_CASE("a domain directory removed and made again with the same id")
{
    TempDomain domain;
    auto writer = std::make_unique<Writer>(domain.dir);
    writer->write(500, 0x11);
    cc::SourceReader source;
    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(firstByte(source.reader(), 500) == 0x11);

    writer->stop();
    std::filesystem::remove_all(domain.dir);
    CHECK_FALSE(source.valid());
    CHECK(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::DomainMissing);
    CHECK(source.instance() == nullptr);

    domain.make();
    CHECK(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::FlowMissing);
    writer = std::make_unique<Writer>(domain.dir);
    writer->write(501, 0x22);
    REQUIRE(source.open(domain.dir.string(), kFlow) == cc::SourceReader::Open::Ok);
    CHECK(firstByte(source.reader(), 501) == 0x22);
    CHECK(source.valid());
}

TEST_CASE("re-open schedule: at once, then 100, 200, 500 ms, then every second")
{
    cc::ReopenSchedule schedule;
    CHECK_FALSE(schedule.active());
    CHECK_FALSE(schedule.due(0));
    schedule.start(10.0);
    CHECK(schedule.active());
    CHECK(schedule.due(10.0));
    double const expected[] = {0.1, 0.2, 0.5, 1.0, 1.0, 1.0};
    double now = 10.0;
    for (double delay : expected)
    {
        CHECK(schedule.failed(now) == doctest::Approx(delay));
        CHECK_FALSE(schedule.due(now + delay - 0.001));
        CHECK(schedule.due(now + delay));
        now += delay;
    }
    CHECK(schedule.attempts() == 6);
    CHECK(schedule.startedAt() == doctest::Approx(10.0));
    schedule.reset();
    CHECK_FALSE(schedule.active());
    CHECK(schedule.attempts() == 0);
}

TEST_CASE("flowFiles: a flow that is back, re-created or whose domain is made again shows as changed")
{
    TempDomain domain;
    auto const dir = domain.dir.string();
    auto const none = cc::flowFiles(dir, kFlow);
    CHECK_FALSE(none.present());
    CHECK(none.domainDef != 0);

    auto writer = std::make_unique<Writer>(domain.dir);
    auto const first = cc::flowFiles(dir, kFlow);
    CHECK(first.present());
    CHECK_FALSE(first == none);
    CHECK(cc::flowFiles(dir, kFlow) == first);

    // Hold a reader on it, as the channel does, so the old inodes stay in use.
    cc::SourceReader source;
    REQUIRE(source.open(dir, kFlow) == cc::SourceReader::Open::Ok);
    writer->stop();
    CHECK_FALSE(cc::flowFiles(dir, kFlow).present());
    writer = std::make_unique<Writer>(domain.dir);
    auto const second = cc::flowFiles(dir, kFlow);
    CHECK(second.present());
    CHECK_FALSE(second == first);

    // The domain directory removed and made again: domain_def.json last.
    writer->stop();
    source.release();
    std::filesystem::remove_all(domain.dir);
    CHECK_FALSE(cc::flowFiles(dir, kFlow).present());
    std::filesystem::create_directories(domain.dir);
    std::ofstream(domain.dir / "options.json") << "{\"urn:x-mxl:option:history_duration/v1.0\": 200000000}\n";
    std::filesystem::create_directories(domain.dir / (std::string(kFlow) + ".mxl-flow"));
    std::ofstream(domain.dir / (std::string(kFlow) + ".mxl-flow") / "data") << "x";
    auto const noDef = cc::flowFiles(dir, kFlow);
    CHECK_FALSE(noDef.present());
    std::ofstream(domain.dir / "domain_def.json") << "{\"id\":\"5e0c0c0c-0000-4000-8000-0000000000d0\"}\n";
    auto const withDef = cc::flowFiles(dir, kFlow);
    CHECK(withDef.present());
    CHECK_FALSE(withDef == noDef);
    CHECK(cc::flowFiles("", kFlow) == cc::FlowFiles{});
}
