file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}" "// Generated — do not edit.
#pragma once
#include <string_view>
namespace cc::webui {
inline constexpr unsigned char kIndex[] = {${bytes}};
inline std::string_view indexHtml() {
    return {reinterpret_cast<char const*>(kIndex), sizeof(kIndex)};
}
}
")
