#pragma once

// BT.709 narrow-range Y′CbCr ↔ R′G′B′.
// Codes: Y 64–940 (span 876), Cb/Cr centred on 512 with a nominal span of 896.
// Normalised y is 0 at black and 1 at white. Normalised cb/cr are 0 at 512
// and ±0.5 at the nominal chroma limits.

namespace cc::bt709
{

inline constexpr double kKr = 0.2126;
inline constexpr double kKg = 0.7152;
inline constexpr double kKb = 0.0722;
inline constexpr double kCrScale = 2.0 * (1.0 - kKr);                 // 1.5748
inline constexpr double kCbScale = 2.0 * (1.0 - kKb);                 // 1.8556
inline constexpr double kCbToG = 2.0 * kKb * (1.0 - kKb) / kKg;       // 0.1873242728
inline constexpr double kCrToG = 2.0 * kKr * (1.0 - kKr) / kKg;       // 0.4681242728
inline constexpr double kYBlack = 64.0;
inline constexpr double kYSpan = 876.0;
inline constexpr double kCZero = 512.0;
inline constexpr double kCSpan = 896.0;

inline void codeToN(double Y, double Cb, double Cr, double& y, double& cb, double& cr)
{
    y = (Y - kYBlack) / kYSpan;
    cb = (Cb - kCZero) / kCSpan;
    cr = (Cr - kCZero) / kCSpan;
}

inline void nToCode(double y, double cb, double cr, double& Y, double& Cb, double& Cr)
{
    Y = y * kYSpan + kYBlack;
    Cb = cb * kCSpan + kCZero;
    Cr = cr * kCSpan + kCZero;
}

inline void nToRgb(double y, double cb, double cr, double& R, double& G, double& B)
{
    R = y + kCrScale * cr;
    G = y - kCbToG * cb - kCrToG * cr;
    B = y + kCbScale * cb;
}

inline void rgbToN(double R, double G, double B, double& y, double& cb, double& cr)
{
    y = kKr * R + kKg * G + kKb * B;
    cb = (B - y) / kCbScale;
    cr = (R - y) / kCrScale;
}

inline void codeToRgb(double Y, double Cb, double Cr, double& R, double& G, double& B)
{
    double y, cb, cr;
    codeToN(Y, Cb, Cr, y, cb, cr);
    nToRgb(y, cb, cr, R, G, B);
}

inline void rgbToCode(double R, double G, double B, double& Y, double& Cb, double& Cr)
{
    double y, cb, cr;
    rgbToN(R, G, B, y, cb, cr);
    nToCode(y, cb, cr, Y, Cb, Cr);
}

} // namespace cc::bt709
