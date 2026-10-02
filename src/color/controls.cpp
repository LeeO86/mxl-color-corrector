#include "color/controls.hpp"

#include "color/bt709.hpp"

#include <algorithm>
#include <cmath>

namespace cc
{
namespace
{
double clampd(double v, double lo, double hi)
{
    return std::max(lo, std::min(hi, v));
}
} // namespace

bool nearlyEqual(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

bool isNeutral(Controls const& c)
{
    return nearlyEqual(c.white.r, 0) && nearlyEqual(c.white.g, 0) && nearlyEqual(c.white.b, 0) && nearlyEqual(c.black.r, 0) &&
           nearlyEqual(c.black.g, 0) && nearlyEqual(c.black.b, 0) && nearlyEqual(c.gain, 100) && nearlyEqual(c.pedestal, 0) &&
           nearlyEqual(c.brightness, 0) && nearlyEqual(c.saturation, 100);
}

ClipMode parseClipMode(std::string const& text, bool& ok)
{
    ok = true;
    if (text == "legal")
    {
        return ClipMode::Legal;
    }
    if (text == "extended")
    {
        return ClipMode::Extended;
    }
    if (text == "off")
    {
        return ClipMode::Off;
    }
    ok = false;
    return ClipMode::Legal;
}

std::string clipModeName(ClipMode mode)
{
    switch (mode)
    {
        case ClipMode::Extended:
            return "extended";
        case ClipMode::Off:
            return "off";
        case ClipMode::Legal:
        default:
            return "legal";
    }
}

std::string validateControls(Controls const& c)
{
    auto range = [](double v, double lo, double hi, char const* name) -> std::string {
        if (v < lo || v > hi)
        {
            return std::string(name) + " out of range";
        }
        return {};
    };
    if (auto e = range(c.white.r, -20, 20, "white.r"); !e.empty()) return e;
    if (auto e = range(c.white.g, -20, 20, "white.g"); !e.empty()) return e;
    if (auto e = range(c.white.b, -20, 20, "white.b"); !e.empty()) return e;
    if (auto e = range(c.black.r, -5, 5, "black.r"); !e.empty()) return e;
    if (auto e = range(c.black.g, -5, 5, "black.g"); !e.empty()) return e;
    if (auto e = range(c.black.b, -5, 5, "black.b"); !e.empty()) return e;
    if (auto e = range(c.whiteWheelX, -1, 1, "white_wheel.x"); !e.empty()) return e;
    if (auto e = range(c.whiteWheelY, -1, 1, "white_wheel.y"); !e.empty()) return e;
    if (auto e = range(c.blackWheelX, -1, 1, "black_wheel.x"); !e.empty()) return e;
    if (auto e = range(c.blackWheelY, -1, 1, "black_wheel.y"); !e.empty()) return e;
    if (auto e = range(c.gain, 0, 200, "gain"); !e.empty()) return e;
    if (auto e = range(c.pedestal, -10, 10, "pedestal"); !e.empty()) return e;
    if (auto e = range(c.brightness, -20, 20, "brightness"); !e.empty()) return e;
    if (auto e = range(c.saturation, 0, 200, "saturation"); !e.empty()) return e;
    return {};
}

void controlsToAffine(Controls const& c, RgbAffine& affine)
{
    double const whites[3] = {c.white.r / 100.0, c.white.g / 100.0, c.white.b / 100.0};
    double const blacks[3] = {c.black.r / 100.0, c.black.g / 100.0, c.black.b / 100.0};
    double const gain = c.gain / 100.0;
    double const pedestal = c.pedestal / 100.0;
    double const brightness = c.brightness / 100.0;
    for (int i = 0; i < 3; ++i)
    {
        double const gainC = gain * (1.0 + whites[i]);
        double const pedC = pedestal + blacks[i];
        affine.a[i] = gainC - pedC;
        affine.b[i] = pedC + brightness;
    }
    affine.saturation = c.saturation / 100.0;
}

void referenceYcbcr(RgbAffine const& affine, double Y, double Cb, double Cr, double& oY, double& oCb, double& oCr)
{
    double y, cb, cr, R, G, B;
    bt709::codeToN(Y, Cb, Cr, y, cb, cr);
    bt709::nToRgb(y, cb, cr, R, G, B);
    R = affine.a[0] * R + affine.b[0];
    G = affine.a[1] * G + affine.b[1];
    B = affine.a[2] * B + affine.b[2];
    bt709::rgbToN(R, G, B, y, cb, cr);
    cb *= affine.saturation;
    cr *= affine.saturation;
    bt709::nToCode(y, cb, cr, oY, oCb, oCr);
}

WheelTrims wheelToTrims(double x, double y, double maxPercent)
{
    double cb = clampd(y, -1.0, 1.0);
    double cr = clampd(x, -1.0, 1.0);
    double const radius = std::hypot(cb, cr);
    if (radius > 1.0)
    {
        cb /= radius;
        cr /= radius;
    }
    // Chroma offsets are already orthogonal to luma. Scale so a full deflection
    // toward blue (the largest primary coefficient) reaches maxPercent.
    double const scale = (maxPercent / 100.0) / bt709::kCbScale;
    WheelTrims trims;
    trims.r = (bt709::kCrScale * cr) * scale * 100.0;
    trims.g = (-bt709::kCbToG * cb - bt709::kCrToG * cr) * scale * 100.0;
    trims.b = (bt709::kCbScale * cb) * scale * 100.0;
    return trims;
}

void trimsToWheel(double r, double g, double b, double maxPercent, double& x, double& y)
{
    (void)g;
    double const scale = (maxPercent / 100.0) / bt709::kCbScale;
    double const cr = (scale == 0) ? 0 : (r / 100.0) / (bt709::kCrScale * scale);
    double const cb = (scale == 0) ? 0 : (b / 100.0) / (bt709::kCbScale * scale);
    x = clampd(cr, -1.0, 1.0);
    y = clampd(cb, -1.0, 1.0);
    (void)b;
}

} // namespace cc
