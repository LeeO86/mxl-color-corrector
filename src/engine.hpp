#pragma once

#include "config.hpp"
#include "metrics.hpp"
#include "nmos.hpp"
#include "store.hpp"

#include <memory>

namespace cc
{

class Engine
{
public:
    Engine(Config config, ControlStore& store, RuntimeBoard& runtime, NmosNode& nmos);
    ~Engine();
    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cc
