// Copyright (C) 2026, the Celestia Development Team

#pragma once

#include <filesystem>
#include <memory>

#include <celutil/classops.h>

namespace celestia::engine
{
class ResourceSystem;
}

namespace celestia::render
{

class BrunetonAtmosphereResource;

class BrunetonAtmosphereManager : private util::NoCopy
{
public:
    explicit BrunetonAtmosphereManager(engine::ResourceSystem&);
    ~BrunetonAtmosphereManager();

    // Render thread only. The returned pointer is owned by the cache and must
    // be looked up again each frame rather than retained by the caller.
    BrunetonAtmosphereResource* find(const std::filesystem::path&);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace celestia::render
