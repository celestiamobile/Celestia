// brunetonatmosphere_test.cpp
//
// Copyright (C) 2026, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest.h>

#include <celrender/brunetonatmosphere.h>

namespace
{

using celestia::render::BrunetonAtmosphereData;
using celestia::render::BrunetonTextureKind;

void writeU32(std::ostream& output, std::uint32_t value)
{
    std::array<char, 4> bytes{
        static_cast<char>(value),
        static_cast<char>(value >> 8),
        static_cast<char>(value >> 16),
        static_cast<char>(value >> 24),
    };
    output.write(bytes.data(), bytes.size());
}

void writeU64(std::ostream& output, std::uint64_t value)
{
    writeU32(output, static_cast<std::uint32_t>(value));
    writeU32(output, static_cast<std::uint32_t>(value >> 32));
}

} // end unnamed namespace

TEST_CASE("Bruneton atmosphere data reads JSON-free packed textures")
{
    auto path = std::filesystem::temp_directory_path() / "celestia-bruneton-test.atm";
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());

    constexpr std::array<char, 8> magic{ 'C', 'E', 'L', 'A', 'T', 'M', '\r', '\n' };
    output.write(magic.data(), magic.size());
    writeU32(output, 1);
    writeU32(output, 0x01020304);
    writeU32(output, 5);
    writeU32(output, 40);

    constexpr std::uint64_t payloadStart = 24 + 5 * 40;
    for (std::uint32_t kind = 1; kind <= 5; ++kind)
    {
        bool is3D = kind == 4 || kind == 5;
        writeU32(output, kind);
        writeU32(output, is3D ? 3 : 2);
        writeU32(output, 1);
        writeU32(output, 1);
        writeU32(output, 1);
        writeU32(output, 1);
        writeU64(output, payloadStart + (kind - 1) * 12);
        writeU64(output, 12);
    }

    for (std::uint32_t kind = 1; kind <= 5; ++kind)
    {
        std::array<float, 3> pixel{
            static_cast<float>(kind),
            static_cast<float>(kind) + 0.25f,
            static_cast<float>(kind) + 0.5f,
        };
        output.write(reinterpret_cast<const char*>(pixel.data()), sizeof(pixel));
    }
    output.close();

    BrunetonAtmosphereData data;
    REQUIRE(data.load(path));
    REQUIRE(data.textures().size() == 5);

    const auto* multiple = data.find(BrunetonTextureKind::MultipleScattering);
    REQUIRE(multiple != nullptr);
    CHECK(multiple->dimensions == 3);
    CHECK(multiple->pixels == std::vector<float>{ 4.0f, 4.25f, 4.5f });

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
