// brunetonatmosphere.cpp
//
// Copyright (C) 2026, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "brunetonatmosphere.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>

#include <celutil/logger.h>

namespace celestia::render
{

namespace
{

constexpr std::array<std::uint8_t, 8> Magic{ 'C', 'E', 'L', 'A', 'T', 'M', '\r', '\n' };
constexpr std::uint32_t FormatVersion = 1;
constexpr std::uint32_t EndianMarker = 0x01020304;
constexpr std::uint32_t HeaderSize = 24;
constexpr std::uint32_t EntrySize = 40;
constexpr std::uint32_t RGB32F = 1;
constexpr std::uint32_t ChannelCount = 3;
constexpr std::uint32_t MaximumDimension = 16384;

std::uint32_t
readU32(const std::uint8_t* value)
{
    return static_cast<std::uint32_t>(value[0]) |
           static_cast<std::uint32_t>(value[1]) << 8 |
           static_cast<std::uint32_t>(value[2]) << 16 |
           static_cast<std::uint32_t>(value[3]) << 24;
}

std::uint64_t
readU64(const std::uint8_t* value)
{
    return static_cast<std::uint64_t>(readU32(value)) |
           static_cast<std::uint64_t>(readU32(value + 4)) << 32;
}

bool
isKnownKind(BrunetonTextureKind kind)
{
    auto value = static_cast<std::uint32_t>(kind);
    return value >= static_cast<std::uint32_t>(BrunetonTextureKind::Phase) &&
           value <= static_cast<std::uint32_t>(BrunetonTextureKind::ThetaDeviation);
}

bool
isThreeDimensional(BrunetonTextureKind kind)
{
    return kind == BrunetonTextureKind::MultipleScattering ||
           kind == BrunetonTextureKind::SingleAerosolsScattering;
}

struct Entry
{
    BrunetonTextureKind kind;
    std::uint32_t dimensions;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint64_t offset;
    std::uint64_t size;
};

} // end unnamed namespace

bool
BrunetonAtmosphereData::load(const std::filesystem::path& path)
{
    m_textures.clear();
    auto fail = [&path](std::string_view message)
    {
        celestia::util::GetLogger()->error(
            "Failed to load Bruneton atmosphere {}: {}.\n", path, message);
        return false;
    };

    std::error_code ec;
    std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec)
        return fail("could not determine file size");

    std::ifstream input(path, std::ios::binary);
    if (!input.good())
        return fail("could not open file");

    std::array<std::uint8_t, HeaderSize> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!input || !std::equal(Magic.begin(), Magic.end(), header.begin()))
        return fail("invalid or truncated header");

    if (readU32(header.data() + 8) != FormatVersion)
        return fail("unsupported format version");

    if (readU32(header.data() + 12) != EndianMarker)
        return fail("invalid endian marker");

    std::uint32_t textureCount = readU32(header.data() + 16);
    if ((textureCount != 5 && textureCount != 6) ||
        readU32(header.data() + 20) != EntrySize)
        return fail("invalid texture directory");

    std::uint64_t directoryEnd = HeaderSize + static_cast<std::uint64_t>(textureCount) * EntrySize;
    if (directoryEnd > fileSize)
        return fail("truncated texture directory");

    std::vector<Entry> entries;
    entries.reserve(textureCount);
    std::array<bool, 7> seen{};
    for (std::uint32_t i = 0; i < textureCount; ++i)
    {
        std::array<std::uint8_t, EntrySize> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!input)
            return fail("truncated texture directory");

        Entry entry{
            static_cast<BrunetonTextureKind>(readU32(bytes.data())),
            readU32(bytes.data() + 4),
            readU32(bytes.data() + 8),
            readU32(bytes.data() + 12),
            readU32(bytes.data() + 16),
            readU64(bytes.data() + 24),
            readU64(bytes.data() + 32),
        };

        auto kindIndex = static_cast<std::uint32_t>(entry.kind);
        if (!isKnownKind(entry.kind) || seen[kindIndex])
            return fail("unknown or duplicate texture kind");
        seen[kindIndex] = true;

        std::uint32_t expectedDimensions = isThreeDimensional(entry.kind) ? 3 : 2;
        if (entry.dimensions != expectedDimensions ||
            entry.width == 0 || entry.width > MaximumDimension ||
            entry.height == 0 || entry.height > MaximumDimension ||
            entry.depth == 0 || entry.depth > MaximumDimension ||
            (entry.dimensions == 2 && entry.depth != 1) ||
            readU32(bytes.data() + 20) != RGB32F)
            return fail("invalid texture descriptor");

        constexpr std::uint64_t BytesPerTexel = ChannelCount * sizeof(float);
        if (entry.width > std::numeric_limits<std::uint64_t>::max() / entry.height ||
            static_cast<std::uint64_t>(entry.width) * entry.height >
                std::numeric_limits<std::uint64_t>::max() / entry.depth)
            return fail("texture dimensions overflow");

        std::uint64_t texelCount = static_cast<std::uint64_t>(entry.width) *
                                   entry.height * entry.depth;
        if (texelCount > std::numeric_limits<std::uint64_t>::max() / BytesPerTexel ||
            entry.size != texelCount * BytesPerTexel ||
            entry.offset < directoryEnd ||
            entry.offset > fileSize ||
            entry.size > fileSize - entry.offset)
            return fail("invalid texture payload");

        entries.push_back(entry);
    }

    if (!seen[static_cast<std::size_t>(BrunetonTextureKind::Phase)] ||
        !seen[static_cast<std::size_t>(BrunetonTextureKind::Transmittance)] ||
        !seen[static_cast<std::size_t>(BrunetonTextureKind::IndirectIlluminance)] ||
        !seen[static_cast<std::size_t>(BrunetonTextureKind::MultipleScattering)] ||
        !seen[static_cast<std::size_t>(BrunetonTextureKind::SingleAerosolsScattering)])
        return fail("missing required texture");

    auto sortedEntries = entries;
    std::sort(sortedEntries.begin(), sortedEntries.end(),
              [](const Entry& lhs, const Entry& rhs) { return lhs.offset < rhs.offset; });
    for (std::size_t i = 1; i < sortedEntries.size(); ++i)
    {
        if (sortedEntries[i - 1].offset + sortedEntries[i - 1].size > sortedEntries[i].offset)
            return fail("overlapping texture payloads");
    }

    m_textures.reserve(entries.size());
    const std::uint16_t endianTest = 1;
    const bool hostIsLittleEndian = *reinterpret_cast<const std::uint8_t*>(&endianTest) == 1;
    for (const Entry& entry : entries)
    {
        BrunetonTextureData texture{
            entry.kind,
            entry.dimensions,
            entry.width,
            entry.height,
            entry.depth,
            std::vector<float>(entry.size / sizeof(float)),
        };

        input.seekg(static_cast<std::streamoff>(entry.offset));
        input.read(reinterpret_cast<char*>(texture.pixels.data()),
                   static_cast<std::streamsize>(entry.size));
        if (!input)
        {
            m_textures.clear();
            return fail("could not read texture payload");
        }

        if (!hostIsLittleEndian)
        {
            for (float& value : texture.pixels)
            {
                auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
                std::swap(bytes[0], bytes[3]);
                std::swap(bytes[1], bytes[2]);
            }
        }

        m_textures.push_back(std::move(texture));
    }

    return true;
}

const BrunetonTextureData*
BrunetonAtmosphereData::find(BrunetonTextureKind kind) const noexcept
{
    auto it = std::find_if(m_textures.begin(), m_textures.end(),
                           [kind](const BrunetonTextureData& texture)
                           {
                               return texture.kind == kind;
                           });
    return it == m_textures.end() ? nullptr : &*it;
}

} // namespace celestia::render
