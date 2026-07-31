// Copyright (C) 2026, the Celestia Development Team

#include "brunetonatmospheremanager.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <celengine/asyncresourcecache.h>
#include <celengine/brunetonatmospherefile.h>
#include <celengine/resourcesystem.h>
#include <celutil/fsutils.h>
#include <celutil/logger.h>

#include "brunetonatmosphereresource.h"

namespace celestia::render
{

namespace
{

enum class AtmosphereHandle : std::uint32_t
{
    Invalid = ~UINT32_C(0),
};

struct AtmosphereInfo
{
    std::filesystem::path path;
};

class AtmospherePaths
{
public:
    AtmosphereHandle getHandle(const std::filesystem::path& path)
    {
        if (path.empty())
            return AtmosphereHandle::Invalid;

        auto [iter, inserted] =
            m_handles.try_emplace(path, static_cast<AtmosphereHandle>(m_paths.size()));
        if (inserted)
            m_paths.push_back(path);
        return iter->second;
    }

    bool getInfo(AtmosphereHandle handle, AtmosphereInfo& info) const
    {
        const auto index = static_cast<std::size_t>(handle);
        if (index >= m_paths.size())
            return false;
        info.path = m_paths[index];
        return true;
    }

private:
    std::vector<std::filesystem::path> m_paths;
    std::unordered_map<std::filesystem::path,
                       AtmosphereHandle,
                       util::PathHasher> m_handles;
};

class AtmosphereTraits
{
public:
    using Handle = AtmosphereHandle;
    using Info = AtmosphereInfo;
    using CpuData = engine::BrunetonAtmosphereData;
    using GpuResource = BrunetonAtmosphereResource;

    explicit AtmosphereTraits(std::shared_ptr<const AtmospherePaths> paths) :
        m_paths(std::move(paths))
    {
    }

    bool getInfo(Handle handle, Info& info) const
    {
        return m_paths->getInfo(handle, info);
    }

    std::optional<CpuData> decode(const Info& info) const
    {
        CpuData data;
        std::string error;
        if (!engine::LoadBrunetonAtmosphere(info.path, data, error))
        {
            util::GetLogger()->error(
                "Failed to load Bruneton atmosphere {}: {}.\n",
                info.path,
                error);
            return std::nullopt;
        }
        return data;
    }

    std::unique_ptr<GpuResource> upload(CpuData&& data) const
    {
        auto resource = std::make_unique<GpuResource>();
        if (!resource->upload(data))
            return nullptr;
        return resource;
    }

    std::size_t gpuBytes(const GpuResource& resource) const noexcept
    {
        return resource.gpuBytes();
    }

    GpuResource* placeholder() const noexcept { return nullptr; }

private:
    std::shared_ptr<const AtmospherePaths> m_paths;
};

} // namespace

struct BrunetonAtmosphereManager::Impl
{
    explicit Impl(engine::ResourceSystem& system) :
        paths(std::make_shared<AtmospherePaths>()),
        cache(system, AtmosphereTraits(paths))
    {
    }

    std::shared_ptr<AtmospherePaths> paths;
    engine::AsyncResourceCache<AtmosphereTraits> cache;
};

BrunetonAtmosphereManager::BrunetonAtmosphereManager(
    engine::ResourceSystem& system) :
    m_impl(std::make_unique<Impl>(system))
{
}

BrunetonAtmosphereManager::~BrunetonAtmosphereManager() = default;

BrunetonAtmosphereResource*
BrunetonAtmosphereManager::find(const std::filesystem::path& path)
{
    return m_impl->cache.find(m_impl->paths->getHandle(path));
}

} // namespace celestia::render
