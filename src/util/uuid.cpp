#include "util/uuid.hpp"

#include "util/sha1.hpp"

namespace cc
{
namespace
{
int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::optional<std::array<std::uint8_t, 16>> parseUuid(std::string_view text)
{
    if (text.size() != 36)
    {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> out{};
    std::size_t byte = 0;
    for (std::size_t i = 0; i < text.size();)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (text[i] != '-')
            {
                return std::nullopt;
            }
            ++i;
            continue;
        }
        int const hi = hexVal(text[i]);
        int const lo = hexVal(text[i + 1]);
        if (hi < 0 || lo < 0 || byte >= out.size())
        {
            return std::nullopt;
        }
        out[byte++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    if (byte != 16)
    {
        return std::nullopt;
    }
    return out;
}

std::string formatUuid(std::array<std::uint8_t, 16> const& bytes)
{
    static char const* hex = "0123456789abcdef";
    std::string out = "00000000-0000-0000-0000-000000000000";
    int positions[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 14, 15, 16, 17, 19, 20, 21, 22, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35};
    for (int i = 0; i < 16; ++i)
    {
        out[static_cast<std::size_t>(positions[i * 2])] = hex[bytes[static_cast<std::size_t>(i)] >> 4];
        out[static_cast<std::size_t>(positions[i * 2 + 1])] = hex[bytes[static_cast<std::size_t>(i)] & 0xf];
    }
    return out;
}

bool isUuid(std::string_view text)
{
    return parseUuid(text).has_value();
}

std::string uuidV5(std::string_view namespaceUuid, std::string_view name)
{
    auto ns = parseUuid(namespaceUuid);
    if (!ns)
    {
        ns = parseUuid(kUuidNamespaceUrl);
    }
    std::string data(reinterpret_cast<char const*>(ns->data()), ns->size());
    data.append(name.data(), name.size());
    auto dig = sha1(data);
    std::array<std::uint8_t, 16> bytes{};
    for (int i = 0; i < 16; ++i)
    {
        bytes[static_cast<std::size_t>(i)] = dig[static_cast<std::size_t>(i)];
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return formatUuid(bytes);
}

std::string uuidV5(std::string_view name)
{
    return uuidV5(kUuidNamespaceUrl, name);
}

} // namespace cc
