// loader_validation_test.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include <doctest.h>

#include <celengine/texture.h>
#include <celengine/texturetraits.h>
#include <celengine/glsupport.h>
#include <celengine/virtualtex.h>
#include <celimage/image.h>
#include <celimage/imageformats.h>

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

void
writeFloatDDS(const std::filesystem::path& path,
              std::uint32_t width = 3, std::uint32_t height = 2,
              std::uint32_t mipLevels = 2)
{
    std::ofstream out(path, std::ios::binary);
    out.write("DDS ", 4);
    std::array<std::uint32_t, 31> header{};
    header[0] = 124;
    header[1] = 0x2100f;
    header[2] = height;
    header[3] = width;
    header[4] = width * 16;
    header[6] = mipLevels;
    header[18] = 32;
    header[19] = 4;
    header[20] = 0x30315844; // DX10
    header[26] = 0x401008;
    for (auto value : header)
        writeLE32(out, value);
    for (auto value : { 2u, 3u, 0u, 1u, 0u })
        writeLE32(out, value);
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        const auto count = std::max(width >> mip, 1u) * std::max(height >> mip, 1u) * 4;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            float value = (i + 1.0f) * 0.0001234567f + mip;
            std::uint32_t bits;
            std::memcpy(&bits, &value, sizeof bits);
            writeLE32(out, bits);
        }
    }
}

void
replaceDDSWord(const std::filesystem::path& path, std::streamoff offset, std::uint32_t value)
{
    std::fstream out(path, std::ios::in | std::ios::out | std::ios::binary);
    out.seekp(offset);
    writeLE32(out, value);
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
    CHECK(image->getFormat() == celestia::engine::PixelFormat::sRGBA);
    CHECK(image->getBytesPerPixel() == 4);
    CHECK(image->getMipLevelSize(0) == 4);
    CHECK_FALSE(image->isFloatingPoint());
}

TEST_CASE("Legacy compressed DDS keeps native blocks or byte software fallback")
{
    using namespace celestia;
    TemporaryDirectory directory;
    auto path = directory.getPath("legacy-dxt3.dds");
    writeDDS(path, 4, 4, 1, 0x33545844, 16);
    for (bool native : { false, true })
    {
        const bool s3tc = gl::EXT_texture_compression_s3tc;
        const bool s3tcSRGB = gl::EXT_texture_compression_s3tc_srgb;
        gl::EXT_texture_compression_s3tc = native;
        gl::EXT_texture_compression_s3tc_srgb = native;
        auto image = engine::Image::load(path);
        gl::EXT_texture_compression_s3tc = s3tc;
        gl::EXT_texture_compression_s3tc_srgb = s3tcSRGB;
        REQUIRE(image != nullptr);
        CHECK(image->isCompressed() == native);
        CHECK_FALSE(image->isFloatingPoint());
        CHECK(image->getFormat() == (native ? engine::PixelFormat::DXT3_sRGBA
                                           : engine::PixelFormat::sRGBA));
        CHECK(image->getMipLevelSize(0) == (native ? 16 : 64));
    }
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

TEST_CASE("DDS RGBA32F preserves native float samples and mip strides")
{
    using namespace celestia::engine;
    TemporaryDirectory directory;
    auto path = directory.getPath("float.dds");
    writeFloatDDS(path);
    auto image = Image::load(path);
    REQUIRE(image != nullptr);
    CHECK(image->isValid());
    CHECK(image->getFormat() == PixelFormat::RGBA32F);
    CHECK(image->isFloatingPoint());
    CHECK_FALSE(image->isCompressed());
    CHECK(image->hasAlpha());
    CHECK(image->getComponents() == 4);
    CHECK(image->getBytesPerPixel() == 16);
    CHECK(image->getPitch() == 48);
    CHECK(image->getMipLevelCount() == 2);
    CHECK(image->getMipLevelSize(0) == 96);
    CHECK(image->getMipLevelSize(1) == 16);
    CHECK(image->getMipLevel(1) == image->getPixels() + 96);
    image->forceLinear();
    CHECK(image->getFormat() == PixelFormat::RGBA32F);
    for (int mip = 0; mip < 2; ++mip)
    {
        for (int i = 0; i < image->getMipLevelSize(mip) / 4; ++i)
        {
            float actual;
            std::memcpy(&actual, image->getMipLevel(mip) + i * 4, sizeof actual);
            CHECK(actual == (i + 1.0f) * 0.0001234567f + mip);
        }
    }
    CHECK(image->computeNormalMap(1.0f, true) == nullptr);
    CHECK_FALSE(image->save(directory.getPath("float.png"), ContentType::PNG));
    CHECK_FALSE(image->save(directory.getPath("float.jpg"), ContentType::JPEG));
    CHECK_FALSE(std::filesystem::exists(directory.getPath("float.png")));
    CHECK_FALSE(std::filesystem::exists(directory.getPath("float.jpg")));
}

TEST_CASE("DDS RGBA32F rejects unsupported layouts and truncation")
{
    using namespace celestia::engine;
    TemporaryDirectory directory;
    auto path = directory.getPath("invalid-float.dds");
    writeFloatDDS(path);
    SUBCASE("array") { replaceDDSWord(path, 140, 2); }
    SUBCASE("empty array") { replaceDDSWord(path, 140, 0); }
    SUBCASE("cube") { replaceDDSWord(path, 136, 4); }
    SUBCASE("legacy cube") { replaceDDSWord(path, 112, 0x200); }
    SUBCASE("volume") { replaceDDSWord(path, 132, 4); }
    SUBCASE("1D") { replaceDDSWord(path, 132, 2); }
    SUBCASE("depth") { replaceDDSWord(path, 24, 2); }
    SUBCASE("padded pitch") { replaceDDSWord(path, 20, 64); }
    SUBCASE("missing flagged pitch") { replaceDDSWord(path, 20, 0); }
    SUBCASE("bad surface size") { replaceDDSWord(path, 4, 120); }
    SUBCASE("bad pixel format size") { replaceDDSWord(path, 76, 28); }
    SUBCASE("missing FourCC flag") { replaceDDSWord(path, 80, 0); }
    SUBCASE("unsupported half float") { replaceDDSWord(path, 128, 10); }
    SUBCASE("truncated DX10 header") { std::filesystem::resize_file(path, 147); }
    SUBCASE("truncated base level") { std::filesystem::resize_file(path, 243); }
    SUBCASE("truncated final mip") { std::filesystem::resize_file(path, 259); }
    SUBCASE("oversized storage")
    {
        replaceDDSWord(path, 12, 16384);
        replaceDDSWord(path, 16, 16384);
        replaceDDSWord(path, 20, 16384 * 16);
    }
    CHECK(Image::load(path) == nullptr);
}

TEST_CASE("DDS RGBA32F accepts implicit tight pitch and a single base level")
{
    TemporaryDirectory directory;
    auto path = directory.getPath("float-base.dds");
    writeFloatDDS(path, 3, 1, 1);
    replaceDDSWord(path, 8, 0x1007);
    replaceDDSWord(path, 20, 0);
    replaceDDSWord(path, 28, 0);
    auto image = celestia::engine::Image::load(path);
    REQUIRE(image != nullptr);
    CHECK(image->getMipLevelCount() == 1);
    CHECK(image->getMipLevelSize(0) == 48);
}

TEST_CASE("Float image storage rejects overflow and uses each mip row pitch")
{
    using namespace celestia::engine;
    Image large(PixelFormat::RGBA32F, 16384, 16384);
    CHECK_FALSE(large.isValid());
    Image image(PixelFormat::RGBA32F, 8, 8, 4);
    REQUIRE(image.isValid());
    CHECK(image.getPitch() == 128);
    CHECK(image.getMipLevelSize(0) == 1024);
    CHECK(image.getMipLevelSize(1) == 256);
    CHECK(image.getMipLevelSize(2) == 64);
    CHECK(image.getMipLevelSize(3) == 16);
    CHECK(image.getPixelRow(1, 1) == image.getMipLevel(1) + 64);
    CHECK(image.getPixelRow(2, 1) == image.getMipLevel(2) + 32);
    CHECK(image.getPixelRow(-1, 0) == nullptr);
    CHECK(image.getPixelRow(0, -1) == nullptr);
    CHECK(image.getMipLevel(-1) == nullptr);
    CHECK(image.getPixelRow(3, 1) == nullptr);
    Image bytes(PixelFormat::RGB, 3, 4, 2);
    CHECK(bytes.getBytesPerPixel() == 3);
    CHECK(bytes.getPitch() == 12);
    CHECK(bytes.getMipLevelSize(0) == 48);
    CHECK(bytes.getMipLevelSize(1) == 8);
    CHECK(bytes.getPixelRow(1, 1) == bytes.getMipLevel(1) + 4);
}

TEST_CASE("Float texture upload rejects missing storage or filtering without GL calls")
{
    using namespace celestia;
    const bool storage = gl::textureFloat;
    const bool filtering = gl::textureFloatLinear;
    engine::Image image(engine::PixelFormat::RGBA32F, 1, 1);
    gl::textureFloat = false;
    gl::textureFloatLinear = true;
    CHECK(CreateTextureFromImage(image, Texture::EdgeClamp, Texture::NoMipMaps) == nullptr);
    gl::textureFloat = true;
    gl::textureFloatLinear = false;
    CHECK(CreateTextureFromImage(image, Texture::EdgeClamp, Texture::NoMipMaps) == nullptr);
    gl::textureFloat = storage;
    gl::textureFloatLinear = filtering;
    CHECK(ImageTexture::createProcedural(1, 1, engine::PixelFormat::RGBA32F,
                                        [](float, float, std::uint8_t*) {}) == nullptr);
    CHECK(CubeMap::createProcedural(1, engine::PixelFormat::RGBA32F,
                                   [](float, float, float, std::uint8_t*) {}) == nullptr);
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

TEST_CASE("Single-image optical textures reject virtual and tiled resources")
{
    using namespace celestia;
    using namespace celestia::engine;
    TextureTraits traits({}, nullptr, TextureResolution::medres);
    CHECK_FALSE(traits.decode({"unused.ctx", TextureFlags::SingleTexture, 0.0f}));

    DecodedTexture decoded;
    decoded.image = std::make_unique<Image>(PixelFormat::RGBA32F, 8, 1);
    decoded.singleTexture = true;
    decoded.mipMode = Texture::NoMipMaps;
    const auto originalLimit = gl::maxTextureSize;
    gl::maxTextureSize = 4;
    CHECK(traits.upload(std::move(decoded)) == nullptr);
    gl::maxTextureSize = originalLimit;
}

TEST_SUITE_END();
