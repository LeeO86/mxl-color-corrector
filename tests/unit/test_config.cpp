#include "config.hpp"
#include "store.hpp"
#include "util/json.hpp"
#include "util/uuid.hpp"

#include <cmath>
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

TEST_CASE("platform settings and address aliases")
{
    auto cfg = loadConfig({{"NMOS_SEED", "sport-cc"}, {"NMOS_HOST_ADDRESS", "10.8.0.4"}, {"NMOS_REGISTRY_ADDRESS", "10.1.0.5"}, {"NMOS_REGISTRY_PORT", "4000"},
                              {"NMOS_LABEL", "sport-cc"}, {"NMOS_TAGS", "{\"urn:x-srf:production\":[\"sport-sa\"],\"urn:x-srf:function\":[\"cc1\"]}"}},
        {});
    CHECK(cfg.nmosHostAddress == "10.8.0.4");
    CHECK(cfg.nmosQueryAddress == "10.1.0.5");
    CHECK(cfg.nmosQueryPort == 4001);
    CHECK(cfg.nmosLabel == "sport-cc");
    CHECK(cfg.nmosTags.size() == 2);
    CHECK(cfg.cleanupOnExit == false);
    CHECK(cfg.shutdownTimeoutS == 10);
    CHECK(cfg.historyDurationNs == 200000000);
    auto again = loadConfig({{"NMOS_SEED", "sport-cc"}, {"NMOS_HOST_ADDRESS", "10.9.0.4"}}, {});
    CHECK(cfg.outputDomainId == again.outputDomainId);

    auto aliased = loadConfig({{"HOST_ID", "10.2.0.9"}, {"NMOS_SEED", "s"}}, {});
    CHECK(aliased.nmosHostAddress == "10.2.0.9");
    CHECK(aliased.nmosLabel == "10.2.0.9");
    auto named = loadConfig({{"HOST_ID", "color-a"}, {"NMOS_HOST_ADDRESS", "10.2.0.9"}, {"NMOS_SEED", "s"}}, {});
    CHECK(named.nmosHostAddress == "10.2.0.9");
    CHECK(named.nmosLabel == "color-a");

    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "color.example"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "127.0.0.1"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "0.0.0.0"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_TAGS", "{\"bad\":1}"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"MXL_CLEANUP_ON_EXIT", "maybe"}}, {}), ConfigError);
    CHECK(isAnnouncedAddress("10.1.2.3"));
    CHECK_FALSE(isAnnouncedAddress("not-an-ip"));
}

TEST_CASE("widget frame ancestors and setting origins")
{
    auto cfg = loadConfig({{"WEB_PORT", "8200"}}, {{"CC_CHANNELS", "3"}});
    CHECK(cfg.widgetFrameAncestors == "'self'");
    CHECK(cfg.origins.at("WEB_PORT") == "environment");
    CHECK(cfg.origins.at("CC_CHANNELS") == "file");
    CHECK(cfg.origins.at("CC_CLIP") == "default");
    CHECK(cfg.origins.size() == settingValues(cfg).size());
    CHECK(knownSetting("WIDGET_FRAME_ANCESTORS"));
    auto listed = loadConfig({{"WIDGET_FRAME_ANCESTORS", "'self' https://designer.example"}}, {});
    CHECK(listed.widgetFrameAncestors == "'self' https://designer.example");
    CHECK(loadConfig({{"WIDGET_FRAME_ANCESTORS", "  "}}, {}).widgetFrameAncestors == "'self'");
    CHECK_THROWS_AS(loadConfig({{"WIDGET_FRAME_ANCESTORS", "'self'; script-src *"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"WIDGET_FRAME_ANCESTORS", "https://a.example, https://b.example"}}, {}), ConfigError);
}

TEST_CASE("a wheel keeps the trims' luma, the pots move the wheel, and the trims survive a restart")
{
    auto dir = std::filesystem::temp_directory_path() / "mxl-cc-wheel-test";
    std::filesystem::remove_all(dir);
    auto cfg = loadConfig({{"CC_CHANNELS", "1"}, {"CC_STATE_DIR", dir.string()}}, {});
    auto body = [](char const* text) { return parseJson(text); };
    std::string error;
    RgbTrim white{};
    {
        ControlStore store(cfg);
        // RGB pots all at +10: a white level change, no tint.
        REQUIRE(store.patch(1, body(R"({"white":{"r":10,"g":10,"b":10}})"), error));
        CHECK(std::fabs(store.live(1).whiteWheelX) < 1e-9);
        CHECK(std::fabs(store.live(1).whiteWheelY) < 1e-9);
        // The wheel tints and keeps that luma part.
        REQUIRE(store.patch(1, body(R"({"white_wheel":{"x":0.5,"y":0}})"), error));
        auto c = store.live(1);
        CHECK(trimLuma(c.white) == doctest::Approx(10));
        CHECK(c.whiteWheelX == doctest::Approx(0.5));
        CHECK(std::fabs(c.whiteWheelY) < 1e-9);
        CHECK(c.white.r > c.white.g);
        // One pot moves only its channel, and the wheel follows.
        REQUIRE(store.patch(1, body(R"({"white":{"b":30}})"), error));
        auto d = store.live(1);
        CHECK(d.white.r == doctest::Approx(c.white.r));
        CHECK(d.white.g == doctest::Approx(c.white.g));
        CHECK(d.white.b == 30);
        double x = 0, y = 0;
        trimsToWheel(d.white.r, d.white.g, d.white.b, kWhiteWheelSpan, x, y);
        CHECK(d.whiteWheelX == doctest::Approx(x));
        CHECK(d.whiteWheelY == doctest::Approx(y));
        CHECK(d.whiteWheelY > 0); // more blue
        white = d.white;
        // Numeric trims win over a wheel in the same patch (state files and presets carry both).
        REQUIRE(store.patch(1, body(R"({"black":{"r":100,"g":-100,"b":0},"black_wheel":{"x":0,"y":0}})"), error));
        auto e = store.live(1);
        CHECK(e.black.r == 100);
        CHECK(e.black.g == -100);
        CHECK(std::hypot(e.blackWheelX, e.blackWheelY) == doctest::Approx(1)); // far beyond the black wheel: on the rim
        // A wheel on top of a large luma part stays inside the trim range.
        REQUIRE(store.patch(1, body(R"({"black":{"r":99,"g":99,"b":99}})"), error));
        REQUIRE(store.patch(1, body(R"({"black_wheel":{"x":0,"y":1}})"), error));
        CHECK(store.live(1).black.b == 100);
        CHECK(store.live(1).black.r == doctest::Approx(99));
        // Out of range is refused and changes nothing.
        CHECK_FALSE(store.patch(1, body(R"({"pedestal":100.5})"), error));
        CHECK_FALSE(store.patch(1, body(R"({"white_wheel":{"x":1.5,"y":0}})"), error));
        REQUIRE(store.patch(1, body(R"({"pedestal":-100,"brightness":100})"), error));
    }
    ControlStore again(cfg);
    auto f = again.live(1);
    CHECK(f.white.r == doctest::Approx(white.r));
    CHECK(f.white.g == doctest::Approx(white.g));
    CHECK(f.white.b == 30);
    CHECK(f.black.r == doctest::Approx(99));
    CHECK(f.pedestal == -100);
    CHECK(f.brightness == 100);
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
