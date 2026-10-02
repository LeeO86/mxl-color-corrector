#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cc
{

inline constexpr char kUuidNamespaceUrl[] = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> parseUuid(std::string_view text);
[[nodiscard]] std::string formatUuid(std::array<std::uint8_t, 16> const& bytes);
[[nodiscard]] bool isUuid(std::string_view text);
[[nodiscard]] std::string uuidV5(std::string_view name);
[[nodiscard]] std::string uuidV5(std::string_view namespaceUuid, std::string_view name);

} // namespace cc
