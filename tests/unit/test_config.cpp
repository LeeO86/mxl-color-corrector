#include "config.hpp"
#include "store.hpp"
#include "util/json.hpp"
#include "util/uuid.hpp"

#include <filesystem>

#include "doctest/doctest.h"

using namespace cc;

TEST_CASE("defaults and env override")
{
    auto cfg = loadConfig({}, {});
    CHECK(cfg.channels == 2);
    CHECK(cfg.webPort == 8140);
    CHECK(cfg.nmosPort == 3292);
    CHECK(cfg.clip == ClipMode::Legal);
    CHECK(cfg.rgbClip == false);
    CHECK(cfg.wholeGrainOffset == 1);
    CHECK(cfg.nmosDnsSd == false);
    CHECK(isUuid(cfg.outputDomainId));
    CHECK(cfg.outputDomainDir.find("/cc-") != std::string::npos);

    auto over = loadConfig({{"CC_CHANNELS", "4"}, {"WEB_PORT", "9000"}, {"CC_CLIP", "extended"}, {"CC_RGB_CLIP", "on"}, {"CC_READ_OFFSET_GRAINS", "2"}}, {});
    CHECK(over.channels == 4);
    CHECK(over.webPort == 9000);
    CHECK(over.clip == ClipMode::Extended);
    CHECK(over.rgbClip);
    CHECK(over.wholeGrainOffset == 2);
}

TEST_CASE("env wins over the file and unknown file keys fail")
{
    CHECK_THROWS_AS(loadConfig({}, {{"NOPE", "1"}}), ConfigError);
    auto cfg = loadConfig({{"CC_CHANNELS", "3"}}, {{"CC_CHANNELS", "8"}, {"WEB_PORT", "8200"}});
    CHECK(cfg.channels == 3);
    CHECK(cfg.webPort == 8200);
    CHECK_THROWS_AS(loadConfig({{"CC_CHANNELS", "0"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"CC_CLIP", "full"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"MXL_OUTPUT_DOMAIN_ID", "not-a-uuid"}}, {}), ConfigError);
}

TEST_CASE("state roundtrip and presets")
{
    auto dir = std::filesystem::temp_directory_path() / "mxl-cc-state-test";
    std::filesystem::remove_all(dir);
    auto cfg = loadConfig({{"CC_CHANNELS", "2"}, {"CC_STATE_DIR", dir.string()}, {"CC_CLIP", "legal"}}, {});
    {
        ControlStore store(cfg);
        std::string error;
        Json patch = Json::object();
        patch["gain"] = Json::number(80);
        patch["saturation"] = Json::number(0);
        CHECK(store.patch(1, patch, error));
        CHECK(store.savePreset(1, "warm", error));
        CHECK(store.setSlot(1, "b", error));
        CHECK(store.activeSlot(1) == 'b');
        Json gain = Json::object();
        gain["gain"] = Json::number(140);
        CHECK(store.patch(1, gain, error));
        CHECK(store.setSlot(1, "a", error));
        CHECK(store.live(1).gain == 80);
        CHECK(store.live(1).saturation == 0);
        CHECK(store.reset(1, "gain", error));
        CHECK(store.live(1).gain == 100);
        CHECK(store.live(1).saturation == 0);
        CHECK(store.recallPreset(1, "warm", error));
        CHECK(store.live(1).gain == 80);
        CHECK(store.savePreset(0, "show", error));
    }
    {
        ControlStore store(cfg);
        CHECK(store.live(1).gain == 80);
        CHECK(store.activeSlot(1) == 'a');
        std::string error;
        CHECK(store.setSlot(1, "b", error));
        CHECK(store.live(1).gain == 140);
        auto exported = store.exportPresets();
        CHECK(exported.find("presets") != nullptr);
        CHECK(exported.find("presets")->a.size() == 2);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("uuid v5 is deterministic")
{
    auto a = uuidV5("mxl-color-corrector/demo/node");
    auto b = uuidV5("mxl-color-corrector/demo/node");
    auto c = uuidV5("mxl-color-corrector/demo/device");
    CHECK(a == b);
    CHECK(a != c);
    CHECK(isUuid(a));
}
