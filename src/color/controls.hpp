#pragma once

#include <cstdint>
#include <string>

namespace cc
{

enum class ClipMode
{
    Legal,
    Extended,
    Off
};

struct RgbTrim
{
    double r = 0;
    double g = 0;
    double b = 0;
};

// Limit of the trims, pedestal and brightness, in percent (1.1.0; they were ±20, ±5, ±10 and ±20).
inline constexpr double kOffsetLimit = 100;
// Trim at a colour wheel's full deflection, in percent (unchanged since 1.0.0).
inline constexpr double kWhiteWheelSpan = 20;
inline constexpr double kBlackWheelSpan = 5;

// Operator-facing units. Trims, pedestal and brightness are percents of the
// nominal range. Gain and saturation are percents of unity (100 = unchanged).
struct Controls
{
    RgbTrim white{};                 // −100 … +100
    RgbTrim black{};                 // −100 … +100
    double whiteWheelX = 0;          // −1 … +1, +x towards red; read back from the trims
    double whiteWheelY = 0;          // −1 … +1, +y towards blue
    double blackWheelX = 0;
    double blackWheelY = 0;
    double gain = 100;               // 0 … 200
    double pedestal = 0;             // −100 … +100
    double brightness = 0;           // −100 … +100
    double saturation = 100;         // 0 … 200
    bool bypass = false;
    ClipMode clip = ClipMode::Legal;
    bool rgbClip = false;
};

struct RgbAffine
{
    double a[3]{1, 1, 1}; // out = a * in + b, in normalised RGB
    double b[3]{0, 0, 0};
    double saturation = 1;
};

[[nodiscard]] bool nearlyEqual(double a, double b, double eps = 1e-9);
[[nodiscard]] bool isNeutral(Controls const& controls);
[[nodiscard]] ClipMode parseClipMode(std::string const& text, bool& ok);
[[nodiscard]] std::string clipModeName(ClipMode mode);
[[nodiscard]] std::string validateControls(Controls const& controls);
void controlsToAffine(Controls const& controls, RgbAffine& affine);

// Reference pipeline in double, no clipping. Codes in, codes out.
void referenceYcbcr(RgbAffine const& affine, double Y, double Cb, double Cr, double& oY, double& oCb, double& oCr);

struct WheelTrims
{
    double r = 0;
    double g = 0;
    double b = 0;
};

// Map a colour-wheel position to luma-neutral RGB trims, in percent.
// x > 0 tints toward red, y > 0 tints toward blue. Radius is limited to 1.
[[nodiscard]] WheelTrims wheelToTrims(double x, double y, double maxPercent);
// The luma part of RGB trims (what they add to Y′). The wheel does not show it and does not change it.
[[nodiscard]] double trimLuma(RgbTrim const& trims);
// The wheel position of the trims' chroma part, the inverse of wheelToTrims. A tint beyond the
// wheel's reach stays on the rim in its direction.
void trimsToWheel(double r, double g, double b, double maxPercent, double& x, double& y);

} // namespace cc
