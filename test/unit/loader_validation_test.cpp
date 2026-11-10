// loader_validation_test.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <doctest.h>

#include <celengine/texture.h>
#include <celengine/texmanager.h>
#include <celengine/virtualtex.h>
#include <celengine/viewporteffect.h>
#include <celengine/warpmesh.h>
#include <celimage/image.h>
#include <celimage/imageformats.h>
#include <celestia/celestiacore.h>
#include <celestia/configfile.h>
#include <celutil/logger.h>

namespace
{

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        std::random_device random;
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            path = base / ("celestia-loader-validation-" +
                           std::to_string(random()) + "-" +
                           std::to_string(random()));
            std::error_code ec;
            if (std::filesystem::create_directory(path, ec))
                return;
        }

        throw std::runtime_error("Failed to create temporary test directory");
    }

    ~TemporaryDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path getPath(std::string_view name) const
    {
        return path / name;
    }

private:
    std::filesystem::path path;
};

void
writeLE32(std::ostream& out, std::uint32_t value)
{
    std::array<char, 4> bytes{
        static_cast<char>(value),
        static_cast<char>(value >> 8),
        static_cast<char>(value >> 16),
        static_cast<char>(value >> 24),
    };
    out.write(bytes.data(), bytes.size());
}

void
writeDDS(const std::filesystem::path& path,
         std::uint32_t width,
         std::uint32_t height,
         std::uint32_t mipLevels,
         std::uint32_t fourCC,
         std::size_t payloadSize)
{
    std::ofstream out(path, std::ios::binary);
    out.write("DDS ", 4);

    std::array<std::uint32_t, 31> header{};
    header[0] = 124;
    header[2] = height;
    header[3] = width;
    header[6] = mipLevels;
    header[18] = 32;
    header[20] = fourCC;
    if (fourCC == 0)
    {
        header[4] = width * 4;
        header[21] = 32;
        header[22] = 0x000000ff;
        header[23] = 0x0000ff00;
        header[24] = 0x00ff0000;
        header[25] = 0xff000000;
    }
    for (std::uint32_t value : header)
        writeLE32(out, value);

    std::array<char, 32> payload{};
    REQUIRE(payloadSize <= payload.size());
    out.write(payload.data(), static_cast<std::streamsize>(payloadSize));
}

} // namespace

TEST_SUITE_BEGIN("Loader validation");

TEST_CASE("DDS loader rejects truncated pixel data")
{
    TemporaryDirectory directory;
    auto path = directory.getPath("truncated.dds");
    writeDDS(path, 1, 1, 1, 0, 3);
    std::unique_ptr<celestia::engine::Image> image(
        celestia::engine::LoadDDSImage(path));
    CHECK(image == nullptr);
}

TEST_CASE("DDS loader accepts complete pixel data")
{
    TemporaryDirectory directory;
    auto path = directory.getPath("complete.dds");
    writeDDS(path, 1, 1, 1, 0, 4);
    std::unique_ptr<celestia::engine::Image> image(
        celestia::engine::LoadDDSImage(path));
    REQUIRE(image != nullptr);
    CHECK(image->getWidth() == 1);
    CHECK(image->getHeight() == 1);
}

TEST_CASE("DDS loader rejects excessive mip levels")
{
    TemporaryDirectory directory;
    auto path = directory.getPath("excessive-mips.dds");
    writeDDS(path, 1, 1, 32, 0, 4);
    std::unique_ptr<celestia::engine::Image> image(
        celestia::engine::LoadDDSImage(path));
    CHECK(image == nullptr);
}

TEST_CASE("Virtual texture loader rejects unsafe dimensions")
{
    TemporaryDirectory directory;
    auto path = directory.getPath("invalid.ctx");
    {
        std::ofstream out(path);
        out << "VirtualTexture { ImageDirectory \".\" BaseSplit 18 TileSize 64 }";
    }

    auto texture = LoadVirtualTexture(path, Texture::DefaultColorspace);
    CHECK(texture == nullptr);
}

TEST_CASE("Simulation initialization can retry after a failed star catalog")
{
    TemporaryDirectory directory;
    const auto starPath = directory.getPath("stars.dat");
    const auto configPath = directory.getPath("invalid.cfg");
    const auto fallbackPath = directory.getPath("fallback.cfg");
    {
        std::ofstream out(configPath);
        out << "Configuration { StarDatabase \"" << starPath.generic_string() << "\" }";
        REQUIRE(out.good());
    }
    {
        std::ofstream out(fallbackPath);
        out << "Configuration {}";
        REQUIRE(out.good());
    }

    SUBCASE("Missing star catalog falls back successfully")
    {
        CelestiaCore core;
        REQUIRE_FALSE(core.initSimulation(configPath));
        REQUIRE(core.initSimulation(fallbackPath));
        CHECK(core.getSimulation() != nullptr);
    }
    SUBCASE("Truncated star catalog falls back successfully")
    {
        std::ofstream out(starPath, std::ios::binary);
        out << "CELSTARS";
        REQUIRE(out.good());
        out.close();

        CelestiaCore core;
        REQUIRE_FALSE(core.initSimulation(configPath));
        REQUIRE(core.initSimulation(fallbackPath));
        CHECK(core.getSimulation() != nullptr);
    }
    SUBCASE("Both initialization attempts fail")
    {
        CelestiaCore core;
        REQUIRE_FALSE(core.initSimulation(configPath));
        REQUIRE_FALSE(core.initSimulation(configPath));
        CHECK(core.getSimulation() == nullptr);
    }
}

TEST_CASE("Warp mesh inverse mapping follows the rendered triangles")
{
    celestia::util::CreateLogger();
    TemporaryDirectory directory;
    const auto path = directory.getPath("mesh.data");
    const auto load = [&path](std::string_view data)
    {
        std::ofstream out(path);
        out << data;
        out.close();
        return WarpMesh::load(path);
    };

    SUBCASE("Non-affine UVs and shared triangle edges")
    {
        auto mesh = load("2\n2 2\n-1 -1 0 0 1\n1 -1 1 0.2 1\n"
                         "-1 1 0.2 1 1\n1 1 0.8 0.8 1\n");
        REQUIRE(mesh != nullptr);
        std::vector<Eigen::Vector2f> positions;
        mesh->mapTexture(0.3f, 0.54f, positions);
        REQUIRE(positions.size() == 1);
        CHECK(positions[0].x() == doctest::Approx(-0.6f));
        CHECK(positions[0].y() == doctest::Approx(0.0f).epsilon(1.0e-5));
        positions.clear();
        mesh->mapTexture(0.54f, 0.78f, positions);
        REQUIRE(positions.size() == 1);
        CHECK(positions[0].x() == doctest::Approx(0.0f).epsilon(1.0e-5));
        CHECK(positions[0].y() == doctest::Approx(0.6f));
        positions.clear();
        mesh->mapTexture(0.6f, 0.6f, positions);
        REQUIRE(positions.size() == 1);
        CHECK(positions[0].norm() < 1.0e-5f);
        positions.clear();
        mesh->mapTexture(-0.5f, -0.5f, positions);
        CHECK(positions.empty());
    }
    SUBCASE("Overlapping UV triangles produce distinct output anchors")
    {
        auto mesh = load("2\n2 2\n-1 -1 0 0 1\n1 -1 1 0 1\n"
                         "-1 1 0 1 1\n1 1 0 0 1\n");
        REQUIRE(mesh != nullptr);
        std::vector<Eigen::Vector2f> positions;
        mesh->mapTexture(0.25f, 0.25f, positions);
        REQUIRE(positions.size() == 2);
        CHECK((positions[0] + positions[1]).norm() < 1.0e-5f);
        CHECK(positions[0].norm() == doctest::Approx(std::sqrt(0.5f)));
    }
    SUBCASE("Degenerate UV triangles have no inverse")
    {
        auto mesh = load("2\n2 2\n-1 -1 0 0 1\n1 -1 0 0 1\n"
                         "-1 1 0 0 1\n1 1 0 0 1\n");
        REQUIRE(mesh != nullptr);
        std::vector<Eigen::Vector2f> positions;
        mesh->mapTexture(0, 0, positions);
        CHECK(positions.empty());
    }
    SUBCASE("Spatial index traverses multiple branches")
    {
        {
            std::ofstream out(path);
            out << "2\n5 5\n";
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 5; ++x)
                    out << x * 0.5f - 1.0f << ' ' << y * 0.5f - 1.0f << ' '
                        << x * 0.25f << ' ' << y * 0.25f << " 1\n";
            REQUIRE(out.good());
        }
        auto mesh = WarpMesh::load(path);
        REQUIRE(mesh != nullptr);
        std::vector<Eigen::Vector2f> positions;
        mesh->mapTexture(0.375f, 0.625f, positions);
        REQUIRE(positions.size() == 1);
        CHECK(positions[0].x() == doctest::Approx(-0.25f));
        CHECK(positions[0].y() == doctest::Approx(0.25f));
    }
}

TEST_CASE("FSR source sizes propagate backwards without rounding to zero")
{
    FsrViewportEffect upscale(FsrViewportEffect::Pass::Upscale, 1.5f);
    FsrViewportEffect sharpen(FsrViewportEffect::Pass::Sharpen, 0.2f);
    CHECK(upscale.inputSize({1920, 1080}) == Eigen::Vector2i(1280, 720));
    CHECK(upscale.inputSize(sharpen.inputSize({641, 359})) == Eigen::Vector2i(428, 240));
    CHECK(upscale.inputSize({1, 1}) == Eigen::Vector2i(1, 1));
    FsrViewportEffect performance(FsrViewportEffect::Pass::Upscale, 2.0f);
    CHECK(performance.inputSize({3, 5}) == Eigen::Vector2i(2, 3));
    FsrViewportEffect experimental(FsrViewportEffect::Pass::Upscale, 4.0f);
    CHECK(experimental.inputSize({1920, 1080}) == Eigen::Vector2i(480, 270));
    CHECK(experimental.inputSize({3, 5}) == Eigen::Vector2i(1, 2));
}

TEST_CASE("FSR configuration is opt-in and rejects invalid parameters")
{
    celestia::util::CreateLogger();
    TemporaryDirectory directory;
    const auto path = directory.getPath("fsr.cfg");
    CelestiaConfig config;
    celestia::engine::TexturePaths textures;
    std::string settings;
    SUBCASE("Disabled by default")
    {
        settings = "";
    }
    SUBCASE("Configured scale and sharpening")
    {
        settings = "FSR1Scale 1.7 FSR1Sharpness 1.25";
    }
    SUBCASE("Invalid scale and sharpening")
    {
        settings = "FSR1Scale 0.5 FSR1Sharpness -1";
    }
    SUBCASE("Experimental four-times scale")
    {
        settings = "FSR1Scale 4.0";
    }
    SUBCASE("Excessive scale and sharpening")
    {
        settings = "FSR1Scale 4.1 FSR1Sharpness 3";
    }
    {
        std::ofstream out(path);
        out << "Configuration { " << settings << " }";
        REQUIRE(out.good());
    }
    REQUIRE(ReadCelestiaConfig(path, config, textures));
    if (settings == "FSR1Scale 1.7 FSR1Sharpness 1.25")
    {
        CHECK(config.renderDetails.output.fsr1Scale == doctest::Approx(1.7f));
        CHECK(config.renderDetails.output.fsr1Sharpness == doctest::Approx(1.25f));
    }
    else if (settings == "FSR1Scale 4.0")
    {
        CHECK(config.renderDetails.output.fsr1Scale == 4.0f);
        CHECK(config.renderDetails.output.fsr1Sharpness == doctest::Approx(0.2f));
    }
    else
    {
        CHECK(config.renderDetails.output.fsr1Scale == 1.0f);
        CHECK(config.renderDetails.output.fsr1Sharpness == doctest::Approx(0.2f));
    }
}

TEST_CASE("Live FSR parameters preserve config defaults and reject invalid updates")
{
    TemporaryDirectory directory;
    const auto path = directory.getPath("fsr-live.cfg");
    {
        std::ofstream out(path);
        out << "Configuration { FSR1Scale 2.0 FSR1Sharpness 0.2 }";
        REQUIRE(out.good());
    }
    CelestiaCore core;
    REQUIRE(core.initSimulation(path));
    REQUIRE(core.setFsr1Scale(1.5f));
    REQUIRE(core.setFsr1Sharpness(1.0f));
    CHECK_FALSE(core.setFsr1Scale(0.5f));
    CHECK_FALSE(core.setFsr1Scale(4.1f));
    CHECK_FALSE(core.setFsr1Scale(std::numeric_limits<float>::infinity()));
    CHECK_FALSE(core.setFsr1Sharpness(-0.1f));
    CHECK_FALSE(core.setFsr1Sharpness(2.1f));
    CHECK_FALSE(core.setFsr1Sharpness(std::numeric_limits<float>::quiet_NaN()));
    CHECK(core.getFsr1Scale() == 1.5f);
    CHECK(core.getFsr1Sharpness() == 1.0f);
    CHECK(core.getConfig()->renderDetails.output.fsr1Scale == 2.0f);
    CHECK(core.getConfig()->renderDetails.output.fsr1Sharpness == doctest::Approx(0.2f));
    REQUIRE(core.setFsr1Scale(1.0f));
    CHECK(core.getFsr1Scale() == 1.0f);
}

TEST_SUITE_END();
