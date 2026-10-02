#pragma once

#include "http.hpp"
#include "metrics.hpp"
#include "nmos.hpp"
#include "store.hpp"

#include <string>

namespace cc
{

struct Services
{
    ControlStore* store = nullptr;
    RuntimeBoard* runtime = nullptr;
    NmosNode* nmos = nullptr;
    std::string const* ui = nullptr;
    bool serveNmos = false;
};

void dispatchHttp(Services const& services, HttpRequest const& req, HttpResponse& res);
[[nodiscard]] std::string eventsHello(Services const& services);
void handleEvent(Services const& services, std::string const& text);

} // namespace cc
