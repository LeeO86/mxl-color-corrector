#include "util/log.hpp"

#include <iostream>
#include <mutex>

namespace cc
{
namespace
{
int gLevel = 1; // 0 error, 1 warn, 2 info
std::mutex gMu;

int levelOf(std::string_view level)
{
    if (level == "error") return 0;
    if (level == "warn" || level == "warning") return 1;
    return 2;
}

void write(int level, char const* name, std::string_view message)
{
    if (level > gLevel) return;
    std::lock_guard lock(gMu);
    std::cerr << "{\"level\":\"" << name << "\",\"msg\":\"";
    for (char c : message)
    {
        if (c == '"' || c == '\\') std::cerr << '\\';
        if (c == '\n')
        {
            std::cerr << "\\n";
            continue;
        }
        std::cerr << c;
    }
    std::cerr << "\"}\n";
}
} // namespace

void setLogLevel(std::string_view level)
{
    gLevel = levelOf(level);
}

void logInfo(std::string_view message)
{
    write(2, "info", message);
}
void logWarn(std::string_view message)
{
    write(1, "warn", message);
}
void logError(std::string_view message)
{
    write(0, "error", message);
}

} // namespace cc
