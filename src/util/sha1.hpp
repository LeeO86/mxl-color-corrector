#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cc
{

[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view data);
[[nodiscard]] std::string sha1Hex(std::string_view data);

} // namespace cc
