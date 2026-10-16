#pragma once

// A channel's video format as its flow_def.json declares it, and the grain geometry MXL gives such
// a flow. An interlaced MXL flow (interlace_mode interlaced_tff or interlaced_bff) declares its
// frame rate (25/1 or 30000/1001) and frame height, but each grain is ONE FIELD: half the lines, at
// twice the declared rate (MXL FlowParser: the grain rate numerator doubled, the payload
// line length × height / 2, totalSlices height / 2). The corrector works per pixel, so it reads a
// field and writes a field: its output flow declares the same interlace_mode and rate.

#include <cstddef>
#include <cstdint>
#include <string>

namespace cc
{

struct VideoFormat
{
    int width = 1920;
    int height = 1080; // frame height (flow_def frame_height)
    int rateNum = 50;  // the declared grain_rate: the frame rate of an interlaced flow
    int rateDen = 1;
    std::string interlace = "progressive";
    std::string colorspace = "BT709";
    std::string media = "video/v210";
    std::string label;
    bool operator==(VideoFormat const& other) const
    {
        return width == other.width && height == other.height && rateNum == other.rateNum && rateDen == other.rateDen && interlace == other.interlace &&
               media == other.media;
    }
};

// interlaced_tff or interlaced_bff: the modes MXL stores as fields. MXL refuses any other mode
// (interlaced_psf too), so no other flow exists.
[[nodiscard]] bool isInterlaced(std::string const& interlaceMode);

// One grain of a flow as MXL lays it out.
struct GrainGeometry
{
    bool interlaced = false;
    int lines = 0;           // lines per grain: the frame height, or half of it for a field
    int lineBytes = 0;       // v210 line length (MXL's, 48-pixel padded)
    std::size_t bytes = 0;   // lines × lineBytes: the grain payload MXL allocates
    int rateNum = 0;         // the grain (index) rate: the field rate of an interlaced flow
    int rateDen = 1;
};
[[nodiscard]] GrainGeometry grainGeometry(VideoFormat const& format);

// Whether a grain of `grainSize` bytes and `totalSlices` slices holds the geometry's lines. A
// smaller grain is never read or written past its end; the channel reports `unsupported`.
[[nodiscard]] bool grainFits(GrainGeometry const& geometry, std::uint32_t grainSize, std::uint16_t totalSlices);

// Parses a v210 flow_def.json. False for anything else (or an interlaced flow of odd height,
// which MXL refuses).
[[nodiscard]] bool parseFlow(std::string const& text, VideoFormat& format);

// The output flow_def.json: the input's geometry, interlace_mode and declared rate, so MXL gives
// it the same grain layout as the input (fields for an interlaced input).
[[nodiscard]] std::string flowJson(std::string const& id, std::string const& label, std::string const& group, VideoFormat const& format,
    std::string const& sourceId, std::string const& deviceId);

} // namespace cc
