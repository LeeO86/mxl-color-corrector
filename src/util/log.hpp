#pragma once

#include <string>
#include <string_view>

namespace cc
{

void setLogLevel(std::string_view level);
void logInfo(std::string_view message);
void logWarn(std::string_view message);
void logError(std::string_view message);

} // namespace cc
