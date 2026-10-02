#include "util/sha1.hpp"

#include <cstring>

namespace cc
{
namespace
{
std::uint32_t rotl(std::uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}
} // namespace

std::array<std::uint8_t, 20> sha1(std::string_view data)
{
    std::uint32_t h0 = 0x67452301u;
    std::uint32_t h1 = 0xEFCDAB89u;
    std::uint32_t h2 = 0x98BADCFEu;
    std::uint32_t h3 = 0x10325476u;
    std::uint32_t h4 = 0xC3D2E1F0u;
    std::uint64_t const bitLen = static_cast<std::uint64_t>(data.size()) * 8u;
    std::string msg(data);
    msg.push_back(static_cast<char>(0x80));
    while ((msg.size() % 64) != 56)
    {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i)
    {
        msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xffu));
    }
    for (std::size_t off = 0; off < msg.size(); off += 64)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
        {
            auto const* p = reinterpret_cast<std::uint8_t const*>(msg.data() + off + static_cast<std::size_t>(i) * 4u);
            w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i)
        {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i)
        {
            std::uint32_t f = 0, k = 0;
            if (i < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            std::uint32_t const temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    std::array<std::uint8_t, 20> out{};
    std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i)
    {
        out[static_cast<std::size_t>(i * 4 + 0)] = static_cast<std::uint8_t>((hs[i] >> 24) & 0xffu);
        out[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::uint8_t>((hs[i] >> 16) & 0xffu);
        out[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>((hs[i] >> 8) & 0xffu);
        out[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::uint8_t>(hs[i] & 0xffu);
    }
    return out;
}

std::string sha1Hex(std::string_view data)
{
    auto const dig = sha1(data);
    static char const* hex = "0123456789abcdef";
    std::string out(40, '0');
    for (int i = 0; i < 20; ++i)
    {
        out[static_cast<std::size_t>(i * 2)] = hex[dig[static_cast<std::size_t>(i)] >> 4];
        out[static_cast<std::size_t>(i * 2 + 1)] = hex[dig[static_cast<std::size_t>(i)] & 0xf];
    }
    return out;
}

} // namespace cc
