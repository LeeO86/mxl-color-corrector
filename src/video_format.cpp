#include "video_format.hpp"

#include "util/json.hpp"
#include "v210.hpp"

#include <algorithm>
#include <sstream>

namespace cc
{

bool isInterlaced(std::string const& interlaceMode)
{
    return interlaceMode == "interlaced_tff" || interlaceMode == "interlaced_bff";
}

GrainGeometry grainGeometry(VideoFormat const& format)
{
    GrainGeometry geometry;
    geometry.interlaced = isInterlaced(format.interlace);
    geometry.lines = geometry.interlaced ? format.height / 2 : format.height;
    geometry.lineBytes = v210Stride(format.width);
    geometry.bytes = static_cast<std::size_t>(std::max(geometry.lines, 0)) * static_cast<std::size_t>(geometry.lineBytes);
    geometry.rateNum = geometry.interlaced ? format.rateNum * 2 : format.rateNum;
    geometry.rateDen = std::max(format.rateDen, 1);
    return geometry;
}

bool grainFits(GrainGeometry const& geometry, std::uint32_t grainSize, std::uint16_t totalSlices)
{
    // v210: one slice per line.
    return geometry.lines > 0 && geometry.bytes > 0 && grainSize >= geometry.bytes && totalSlices == static_cast<std::uint32_t>(geometry.lines);
}

bool parseFlow(std::string const& text, VideoFormat& format)
{
    if (text.empty()) return false;
    try
    {
        auto doc = parseJson(text);
        if (auto const* w = doc.find("frame_width")) format.width = static_cast<int>(w->num());
        if (auto const* h = doc.find("frame_height")) format.height = static_cast<int>(h->num());
        if (auto const* rate = doc.find("grain_rate"); rate && rate->isObject())
        {
            if (auto const* n = rate->find("numerator")) format.rateNum = static_cast<int>(n->num());
            if (auto const* d = rate->find("denominator")) format.rateDen = std::max(1, static_cast<int>(d->num()));
        }
        if (auto const* mode = doc.find("interlace_mode")) format.interlace = mode->text();
        if (auto const* cs = doc.find("colorspace")) format.colorspace = cs->text();
        if (auto const* media = doc.find("media_type")) format.media = media->text();
        if (auto const* label = doc.find("label")) format.label = label->text();
        if (isInterlaced(format.interlace) && format.height % 2 != 0) return false;
        return format.width >= 2 && format.height >= 1 && format.rateNum > 0 && format.media == "video/v210";
    }
    catch (JsonError const&)
    {
        return false;
    }
}

std::string flowJson(std::string const& id, std::string const& label, std::string const& group, VideoFormat const& format, std::string const& sourceId,
    std::string const& deviceId)
{
    std::ostringstream os;
    os << "{\n  \"id\": \"" << id << "\",\n  \"label\": \"" << label << "\",\n  \"description\": \"" << label
       << "\",\n  \"format\": \"urn:x-nmos:format:video\",\n  \"media_type\": \"video/v210\",\n"
       << "  \"tags\": {\"urn:x-nmos:tag:grouphint/v1.0\": [\"" << group << "\"]},\n"
       << "  \"grain_rate\": {\"numerator\": " << format.rateNum << ", \"denominator\": " << format.rateDen << "},\n"
       << "  \"frame_width\": " << format.width << ",\n  \"frame_height\": " << format.height << ",\n"
       << "  \"interlace_mode\": \"" << format.interlace << "\",\n  \"colorspace\": \"" << format.colorspace << "\",\n"
       << "  \"source_id\": \"" << sourceId << "\",\n  \"device_id\": \"" << deviceId << "\",\n  \"parents\": [],\n  \"components\": [\n"
       << "    {\"name\": \"Y\", \"width\": " << format.width << ", \"height\": " << format.height << ", \"bit_depth\": 10},\n"
       << "    {\"name\": \"Cb\", \"width\": " << format.width / 2 << ", \"height\": " << format.height << ", \"bit_depth\": 10},\n"
       << "    {\"name\": \"Cr\", \"width\": " << format.width / 2 << ", \"height\": " << format.height << ", \"bit_depth\": 10}\n  ]\n}\n";
    return os.str();
}

} // namespace cc
