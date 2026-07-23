// Copyright (C) 2026, the Celestia Development Team

#include "brunetonatmospherefile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <utility>

#include <celcompat/bit.h>
#include <celutil/binaryread.h>
#include <celutil/binarywrite.h>

namespace celestia::engine
{

namespace
{

constexpr std::array<char, 8> Magic{ 'C', 'E', 'L', 'A', 'T', 'M', '\r', '\n' };
constexpr std::uint32_t HeaderSize = 24;
constexpr std::uint32_t DirectoryEntrySize = 48;
constexpr std::uint32_t ParameterPayloadSize = 256;
constexpr std::uint32_t ParameterFloatCount = 59;
constexpr std::uint32_t ChannelCount = 4;
constexpr std::uint32_t CombinedScatteringFlag = 1;
constexpr std::uint32_t PrecomputedLuminanceFlag = 2;
constexpr std::uint32_t KnownParameterFlags =
    CombinedScatteringFlag | PrecomputedLuminanceFlag;
constexpr std::uint64_t Alignment = 16;

enum class SectionKind : std::uint32_t
{
    Parameters = 1,
    Transmittance = 2,
    Scattering = 3,
    SingleMie = 4,
    Irradiance = 5,
};

enum class SectionFormat : std::uint32_t
{
    ParametersF32 = 1,
    Rgba32F = 2,
    Rgba16F = 3,
};

struct Section
{
    SectionKind kind;
    SectionFormat format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t channels;
    std::uint64_t offset;
    std::uint64_t size;
};

std::uint64_t
alignUp(std::uint64_t value)
{
    return (value + Alignment - 1) & ~(Alignment - 1);
}

bool
fail(std::string& error, const char* message)
{
    error = message;
    return false;
}

bool
writeZeros(std::ostream& output, std::uint64_t count)
{
    constexpr std::array<char, 16> zeros{};
    while (count != 0)
    {
        auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(count, zeros.size()));
        output.write(zeros.data(), chunk);
        if (!output)
            return false;
        count -= static_cast<std::uint64_t>(chunk);
    }
    return true;
}

bool
writeFloats(std::ostream& output, const float* values, std::size_t count)
{
    using celestia::compat::endian;
    if constexpr (endian::native == endian::little)
    {
        output.write(reinterpret_cast<const char*>(values),
                     static_cast<std::streamsize>(count * sizeof(float)));
        return output.good();
    }
    else
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!celestia::util::writeLE(output, values[i]))
                return false;
        }
        return true;
    }
}

bool
readFloats(std::istream& input, float* values, std::size_t count)
{
    using celestia::compat::endian;
    if constexpr (endian::native == endian::little)
    {
        input.read(reinterpret_cast<char*>(values),
                   static_cast<std::streamsize>(count * sizeof(float)));
        return input.good();
    }
    else
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!celestia::util::readLE(input, values[i]))
                return false;
        }
        return true;
    }
}

std::uint16_t
floatToHalf(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    std::uint32_t mantissa = bits & 0x7fffffu;
    const int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;

    if (exponent <= 0)
    {
        if (exponent < -10)
            return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        std::uint32_t result = mantissa >> shift;
        const std::uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (result & 1u) != 0))
            ++result;
        return static_cast<std::uint16_t>(sign | result);
    }

    std::uint32_t result = sign |
                           (static_cast<std::uint32_t>(exponent) << 10) |
                           (mantissa >> 13);
    const std::uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (result & 1u) != 0))
        ++result;
    return static_cast<std::uint16_t>(result);
}

float
halfToFloat(std::uint16_t value)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
    std::uint32_t exponent = (value >> 10) & 0x1fu;
    std::uint32_t mantissa = value & 0x3ffu;
    std::uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            int normalizedExponent = -14;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                --normalizedExponent;
            }
            mantissa &= 0x3ffu;
            bits = sign |
                   (static_cast<std::uint32_t>(normalizedExponent + 127) << 23) |
                   (mantissa << 13);
        }
    }
    else
    {
        bits = sign | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
    }

    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool
writeHalves(std::ostream& output, const float* values, std::size_t count)
{
    std::vector<std::uint16_t> encoded(count);
    std::transform(values, values + count, encoded.begin(), floatToHalf);
    using celestia::compat::endian;
    if constexpr (endian::native == endian::little)
    {
        output.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size() * sizeof(std::uint16_t)));
        return output.good();
    }
    else
    {
        for (std::uint16_t value : encoded)
        {
            if (!celestia::util::writeLE(output, value))
                return false;
        }
        return true;
    }
}

bool
readHalves(std::istream& input, float* values, std::size_t count)
{
    std::vector<std::uint16_t> encoded(count);
    using celestia::compat::endian;
    if constexpr (endian::native == endian::little)
    {
        input.read(reinterpret_cast<char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size() * sizeof(std::uint16_t)));
        if (!input)
            return false;
    }
    else
    {
        for (std::uint16_t& value : encoded)
        {
            if (!celestia::util::readLE(input, value))
                return false;
        }
    }
    std::transform(encoded.begin(), encoded.end(), values, halfToFloat);
    return true;
}

void
appendLayer(std::array<float, ParameterFloatCount>& values,
            std::size_t& index,
            const BrunetonDensityProfileLayer& layer)
{
    values[index++] = layer.width;
    values[index++] = layer.expTerm;
    values[index++] = layer.expScale;
    values[index++] = layer.linearTerm;
    values[index++] = layer.constantTerm;
}

void
appendVector(std::array<float, ParameterFloatCount>& values,
             std::size_t& index,
             const std::array<float, 3>& vector)
{
    for (float value : vector)
        values[index++] = value;
}

std::array<float, ParameterFloatCount>
encodeParameters(const BrunetonAtmosphereParameters& parameters)
{
    std::array<float, ParameterFloatCount> values{};
    std::size_t index = 0;
    appendVector(values, index, parameters.solarIrradiance);
    values[index++] = parameters.sunAngularRadius;
    values[index++] = parameters.bottomRadius;
    values[index++] = parameters.topRadius;
    for (const auto& layer : parameters.rayleighDensity)
        appendLayer(values, index, layer);
    appendVector(values, index, parameters.rayleighScattering);
    for (const auto& layer : parameters.mieDensity)
        appendLayer(values, index, layer);
    appendVector(values, index, parameters.mieScattering);
    appendVector(values, index, parameters.mieExtinction);
    values[index++] = parameters.miePhaseFunctionG;
    for (const auto& layer : parameters.absorptionDensity)
        appendLayer(values, index, layer);
    appendVector(values, index, parameters.absorptionExtinction);
    appendVector(values, index, parameters.groundAlbedo);
    values[index++] = parameters.muSMin;
    appendVector(values, index, parameters.skySpectralRadianceToLuminance);
    appendVector(values, index, parameters.sunSpectralRadianceToLuminance);
    return values;
}

BrunetonDensityProfileLayer
decodeLayer(const std::array<float, ParameterFloatCount>& values, std::size_t& index)
{
    BrunetonDensityProfileLayer layer;
    layer.width = values[index++];
    layer.expTerm = values[index++];
    layer.expScale = values[index++];
    layer.linearTerm = values[index++];
    layer.constantTerm = values[index++];
    return layer;
}

std::array<float, 3>
decodeVector(const std::array<float, ParameterFloatCount>& values, std::size_t& index)
{
    return { values[index++], values[index++], values[index++] };
}

BrunetonAtmosphereParameters
decodeParameters(const std::array<float, ParameterFloatCount>& values,
                 std::uint32_t flags)
{
    BrunetonAtmosphereParameters parameters;
    std::size_t index = 0;
    parameters.solarIrradiance = decodeVector(values, index);
    parameters.sunAngularRadius = values[index++];
    parameters.bottomRadius = values[index++];
    parameters.topRadius = values[index++];
    for (auto& layer : parameters.rayleighDensity)
        layer = decodeLayer(values, index);
    parameters.rayleighScattering = decodeVector(values, index);
    for (auto& layer : parameters.mieDensity)
        layer = decodeLayer(values, index);
    parameters.mieScattering = decodeVector(values, index);
    parameters.mieExtinction = decodeVector(values, index);
    parameters.miePhaseFunctionG = values[index++];
    for (auto& layer : parameters.absorptionDensity)
        layer = decodeLayer(values, index);
    parameters.absorptionExtinction = decodeVector(values, index);
    parameters.groundAlbedo = decodeVector(values, index);
    parameters.muSMin = values[index++];
    parameters.skySpectralRadianceToLuminance = decodeVector(values, index);
    parameters.sunSpectralRadianceToLuminance = decodeVector(values, index);
    parameters.combinedScattering = (flags & CombinedScatteringFlag) != 0;
    parameters.valueMode = (flags & PrecomputedLuminanceFlag) != 0
                               ? BrunetonLutValueMode::PrecomputedLuminance
                               : BrunetonLutValueMode::Radiance;
    return parameters;
}

bool
validateParameters(const BrunetonAtmosphereParameters& parameters, std::string& error)
{
    const auto values = encodeParameters(parameters);
    if (!std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value); }))
        return fail(error, "atmosphere parameters contain a non-finite value");
    if (parameters.bottomRadius <= 0.0f || parameters.topRadius <= parameters.bottomRadius)
        return fail(error, "atmosphere radii are invalid");
    return true;
}

bool
validateTexture(const BrunetonTextureData& texture,
                std::uint32_t width,
                std::uint32_t height,
                std::uint32_t depth,
                bool halfPrecision,
                std::string& error)
{
    if (texture.width != width || texture.height != height || texture.depth != depth)
        return fail(error, "texture dimensions do not match the Bruneton layout");
    const auto texelCount = static_cast<std::uint64_t>(width) * height * depth;
    if (texture.texels.size() != texelCount * ChannelCount)
        return fail(error, "texture payload size does not match its dimensions");
    if (!std::all_of(texture.texels.begin(), texture.texels.end(),
                     [halfPrecision](float value)
                     {
                         return std::isfinite(value) &&
                                (!halfPrecision || std::abs(value) <= 65504.0f);
                     }))
        return fail(error, "texture contains a value outside its storage format");
    return true;
}

bool
validateData(const BrunetonAtmosphereData& data, std::string& error)
{
    if (!validateParameters(data.parameters, error) ||
        !validateTexture(data.transmittance,
                         BrunetonTransmittanceWidth,
                         BrunetonTransmittanceHeight,
                         1,
                         false,
                         error) ||
        !validateTexture(data.scattering,
                         BrunetonScatteringWidth,
                         BrunetonScatteringHeight,
                         BrunetonScatteringDepth,
                         true,
                         error) ||
        !validateTexture(data.irradiance,
                         BrunetonIrradianceWidth,
                         BrunetonIrradianceHeight,
                         1,
                         false,
                         error))
        return false;

    if (data.parameters.combinedScattering)
    {
        if (!data.singleMie.texels.empty())
            return fail(error, "combined scattering must not contain a single-Mie texture");
    }
    else if (!validateTexture(data.singleMie,
                              BrunetonScatteringWidth,
                              BrunetonScatteringHeight,
                              BrunetonScatteringDepth,
                              true,
                              error))
    {
        return false;
    }
    return true;
}

std::uint64_t
textureBytes(const BrunetonTextureData& texture)
{
    return static_cast<std::uint64_t>(texture.texels.size()) * sizeof(float);
}

bool
writeSection(std::ostream& output, const Section& section)
{
    using celestia::util::writeLE;
    return writeLE(output, static_cast<std::uint32_t>(section.kind)) &&
           writeLE(output, static_cast<std::uint32_t>(section.format)) &&
           writeLE(output, section.width) &&
           writeLE(output, section.height) &&
           writeLE(output, section.depth) &&
           writeLE(output, section.channels) &&
           writeLE(output, section.offset) &&
           writeLE(output, section.size) &&
           writeLE(output, std::uint32_t{ 0 }) &&
           writeLE(output, std::uint32_t{ 0 });
}

bool
readSection(std::istream& input, Section& section, std::string& error)
{
    using celestia::util::readLE;
    std::uint32_t kind;
    std::uint32_t format;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    if (!readLE(input, kind) ||
        !readLE(input, format) ||
        !readLE(input, section.width) ||
        !readLE(input, section.height) ||
        !readLE(input, section.depth) ||
        !readLE(input, section.channels) ||
        !readLE(input, section.offset) ||
        !readLE(input, section.size) ||
        !readLE(input, reserved0) ||
        !readLE(input, reserved1))
        return fail(error, "truncated atmosphere section directory");
    if (reserved0 != 0 || reserved1 != 0)
        return fail(error, "nonzero reserved section field");
    if (kind < static_cast<std::uint32_t>(SectionKind::Parameters) ||
        kind > static_cast<std::uint32_t>(SectionKind::Irradiance))
        return fail(error, "unknown atmosphere section");
    if (format < static_cast<std::uint32_t>(SectionFormat::ParametersF32) ||
        format > static_cast<std::uint32_t>(SectionFormat::Rgba16F))
        return fail(error, "unknown atmosphere section format");
    section.kind = static_cast<SectionKind>(kind);
    section.format = static_cast<SectionFormat>(format);
    return true;
}

const Section*
findSection(const std::vector<Section>& sections, SectionKind kind)
{
    auto iter = std::find_if(sections.begin(), sections.end(),
                             [kind](const Section& section)
                             {
                                 return section.kind == kind;
                             });
    return iter == sections.end() ? nullptr : &*iter;
}

bool
validateSectionDescriptor(const Section& section, std::string& error)
{
    if (section.kind == SectionKind::Parameters)
    {
        if (section.format != SectionFormat::ParametersF32 ||
            section.width != 0 || section.height != 0 || section.depth != 0 ||
            section.channels != 0 || section.size != ParameterPayloadSize)
            return fail(error, "invalid atmosphere parameter section");
        return true;
    }

    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    switch (section.kind)
    {
    case SectionKind::Transmittance:
        width = BrunetonTransmittanceWidth;
        height = BrunetonTransmittanceHeight;
        depth = 1;
        break;
    case SectionKind::Scattering:
    case SectionKind::SingleMie:
        width = BrunetonScatteringWidth;
        height = BrunetonScatteringHeight;
        depth = BrunetonScatteringDepth;
        break;
    case SectionKind::Irradiance:
        width = BrunetonIrradianceWidth;
        height = BrunetonIrradianceHeight;
        depth = 1;
        break;
    case SectionKind::Parameters:
        return false;
    }

    const SectionFormat expectedFormat =
        section.kind == SectionKind::Scattering || section.kind == SectionKind::SingleMie
            ? SectionFormat::Rgba16F
            : SectionFormat::Rgba32F;
    const std::uint32_t componentBytes =
        expectedFormat == SectionFormat::Rgba16F ? sizeof(std::uint16_t) : sizeof(float);
    const auto expectedSize = static_cast<std::uint64_t>(width) * height * depth *
                              ChannelCount * componentBytes;
    if (section.format != expectedFormat ||
        section.width != width || section.height != height || section.depth != depth ||
        section.channels != ChannelCount || section.size != expectedSize)
        return fail(error, "invalid atmosphere texture section");
    return true;
}

bool
seekTo(std::istream& input, std::uint64_t offset)
{
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return false;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    return input.good();
}

bool
readTexture(std::istream& input,
            const Section& section,
            BrunetonTextureData& texture,
            std::string& error)
{
    if (!seekTo(input, section.offset))
        return fail(error, "could not seek to atmosphere texture");
    texture.width = section.width;
    texture.height = section.height;
    texture.depth = section.depth;
    const auto texelCount = static_cast<std::size_t>(section.width) *
                            section.height * section.depth * ChannelCount;
    texture.texels.resize(texelCount);
    const bool read = section.format == SectionFormat::Rgba16F
                          ? readHalves(input, texture.texels.data(), texture.texels.size())
                          : readFloats(input, texture.texels.data(), texture.texels.size());
    if (!read)
        return fail(error, "truncated atmosphere texture");
    return true;
}

} // namespace

bool
SaveBrunetonAtmosphere(std::ostream& output,
                       const BrunetonAtmosphereData& data,
                       std::string& error)
{
    error.clear();
    if (!validateData(data, error))
        return false;

    const std::uint32_t sectionCount = data.parameters.combinedScattering ? 4 : 5;
    std::uint64_t nextOffset = alignUp(HeaderSize +
                                       static_cast<std::uint64_t>(sectionCount) *
                                           DirectoryEntrySize);
    std::vector<Section> sections;
    sections.reserve(sectionCount);
    auto addSection = [&](SectionKind kind,
                          SectionFormat format,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::uint32_t depth,
                          std::uint32_t channels,
                          std::uint64_t size)
    {
        sections.push_back({ kind, format, width, height, depth, channels, nextOffset, size });
        nextOffset = alignUp(nextOffset + size);
    };

    addSection(SectionKind::Parameters, SectionFormat::ParametersF32,
               0, 0, 0, 0, ParameterPayloadSize);
    addSection(SectionKind::Transmittance, SectionFormat::Rgba32F,
               data.transmittance.width, data.transmittance.height,
               data.transmittance.depth, ChannelCount, textureBytes(data.transmittance));
    addSection(SectionKind::Scattering, SectionFormat::Rgba16F,
               data.scattering.width, data.scattering.height,
               data.scattering.depth, ChannelCount, textureBytes(data.scattering) / 2);
    if (!data.parameters.combinedScattering)
    {
        addSection(SectionKind::SingleMie, SectionFormat::Rgba16F,
                   data.singleMie.width, data.singleMie.height,
                   data.singleMie.depth, ChannelCount, textureBytes(data.singleMie) / 2);
    }
    addSection(SectionKind::Irradiance, SectionFormat::Rgba32F,
               data.irradiance.width, data.irradiance.height,
               data.irradiance.depth, ChannelCount, textureBytes(data.irradiance));

    output.write(Magic.data(), Magic.size());
    using celestia::util::writeLE;
    if (!output ||
        !writeLE(output, HeaderSize) ||
        !writeLE(output, sectionCount) ||
        !writeLE(output, DirectoryEntrySize) ||
        !writeLE(output, std::uint32_t{ 0 }))
        return fail(error, "could not write atmosphere header");
    for (const Section& section : sections)
    {
        if (!writeSection(output, section))
            return fail(error, "could not write atmosphere section directory");
    }

    const auto parameterValues = encodeParameters(data.parameters);
    for (const Section& section : sections)
    {
        auto position = output.tellp();
        if (position < 0)
            return fail(error, "could not determine atmosphere output position");
        const auto currentOffset = static_cast<std::uint64_t>(position);
        if (currentOffset > section.offset ||
            !writeZeros(output, section.offset - currentOffset))
            return fail(error, "could not align atmosphere section");

        if (section.kind == SectionKind::Parameters)
        {
            std::uint32_t flags =
                data.parameters.combinedScattering ? CombinedScatteringFlag : 0;
            if (data.parameters.valueMode == BrunetonLutValueMode::PrecomputedLuminance)
                flags |= PrecomputedLuminanceFlag;
            if (!writeLE(output, flags) ||
                !writeLE(output, BrunetonScatteringNuSize) ||
                !writeLE(output, BrunetonScatteringMuSSize) ||
                !writeLE(output, ParameterFloatCount) ||
                !writeFloats(output, parameterValues.data(), parameterValues.size()) ||
                !writeLE(output, std::uint32_t{ 0 }))
                return fail(error, "could not write atmosphere parameters");
            continue;
        }

        const BrunetonTextureData* texture = nullptr;
        switch (section.kind)
        {
        case SectionKind::Transmittance: texture = &data.transmittance; break;
        case SectionKind::Scattering: texture = &data.scattering; break;
        case SectionKind::SingleMie: texture = &data.singleMie; break;
        case SectionKind::Irradiance: texture = &data.irradiance; break;
        case SectionKind::Parameters: break;
        }
        if (texture == nullptr)
            return fail(error, "could not write atmosphere texture");
        const bool written = section.format == SectionFormat::Rgba16F
                                 ? writeHalves(output,
                                               texture->texels.data(),
                                               texture->texels.size())
                                 : writeFloats(output,
                                               texture->texels.data(),
                                               texture->texels.size());
        if (!written)
            return fail(error, "could not write atmosphere texture");
    }
    return true;
}

bool
LoadBrunetonAtmosphere(std::istream& input,
                       BrunetonAtmosphereData& data,
                       std::string& error)
{
    error.clear();
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0)
        return fail(error, "could not determine atmosphere file size");
    const auto fileSize = static_cast<std::uint64_t>(end);
    input.seekg(0, std::ios::beg);

    std::array<char, Magic.size()> magic{};
    input.read(magic.data(), magic.size());
    std::uint32_t directoryOffset;
    std::uint32_t sectionCount;
    std::uint32_t entrySize;
    std::uint32_t reserved;
    using celestia::util::readLE;
    if (!input ||
        !readLE(input, directoryOffset) ||
        !readLE(input, sectionCount) ||
        !readLE(input, entrySize) ||
        !readLE(input, reserved))
        return fail(error, "truncated atmosphere header");
    if (magic != Magic)
        return fail(error, "invalid atmosphere magic");
    if (directoryOffset != HeaderSize || entrySize != DirectoryEntrySize ||
        (sectionCount != 4 && sectionCount != 5) || reserved != 0)
        return fail(error, "invalid atmosphere header");

    const auto directoryEnd = static_cast<std::uint64_t>(directoryOffset) +
                              static_cast<std::uint64_t>(sectionCount) * entrySize;
    if (directoryEnd > fileSize)
        return fail(error, "truncated atmosphere section directory");

    std::vector<Section> sections(sectionCount);
    std::array<bool, 6> seen{};
    for (Section& section : sections)
    {
        if (!readSection(input, section, error) ||
            !validateSectionDescriptor(section, error))
            return false;
        const auto kind = static_cast<std::size_t>(section.kind);
        if (seen[kind])
            return fail(error, "duplicate atmosphere section");
        seen[kind] = true;
        if (section.offset % Alignment != 0 || section.offset < directoryEnd ||
            section.offset > fileSize || section.size > fileSize - section.offset)
            return fail(error, "atmosphere section is outside the file");
    }

    if (!seen[static_cast<std::size_t>(SectionKind::Parameters)] ||
        !seen[static_cast<std::size_t>(SectionKind::Transmittance)] ||
        !seen[static_cast<std::size_t>(SectionKind::Scattering)] ||
        !seen[static_cast<std::size_t>(SectionKind::Irradiance)])
        return fail(error, "missing required atmosphere section");

    auto sortedSections = sections;
    std::sort(sortedSections.begin(), sortedSections.end(),
              [](const Section& lhs, const Section& rhs)
              {
                  return lhs.offset < rhs.offset;
              });
    for (std::size_t i = 1; i < sortedSections.size(); ++i)
    {
        if (sortedSections[i - 1].offset + sortedSections[i - 1].size >
            sortedSections[i].offset)
            return fail(error, "overlapping atmosphere sections");
    }

    const Section* parameterSection = findSection(sections, SectionKind::Parameters);
    if (!seekTo(input, parameterSection->offset))
        return fail(error, "could not seek to atmosphere parameters");
    std::uint32_t flags;
    std::uint32_t nuSize;
    std::uint32_t muSSize;
    std::uint32_t floatCount;
    if (!readLE(input, flags) ||
        !readLE(input, nuSize) ||
        !readLE(input, muSSize) ||
        !readLE(input, floatCount))
        return fail(error, "truncated atmosphere parameters");
    if ((flags & ~KnownParameterFlags) != 0 ||
        nuSize != BrunetonScatteringNuSize ||
        muSSize != BrunetonScatteringMuSSize ||
        floatCount != ParameterFloatCount)
        return fail(error, "invalid atmosphere parameter header");

    const bool combined = (flags & CombinedScatteringFlag) != 0;
    const bool hasSingleMie = seen[static_cast<std::size_t>(SectionKind::SingleMie)];
    if (combined == hasSingleMie || sectionCount != (combined ? 4u : 5u))
        return fail(error, "single-Mie section is inconsistent with combined scattering");

    std::array<float, ParameterFloatCount> values{};
    std::uint32_t parameterReserved;
    if (!readFloats(input, values.data(), values.size()) ||
        !readLE(input, parameterReserved))
        return fail(error, "truncated atmosphere parameters");
    if (parameterReserved != 0)
        return fail(error, "nonzero reserved atmosphere parameter field");

    BrunetonAtmosphereData loaded;
    loaded.parameters = decodeParameters(values, flags);
    if (!validateParameters(loaded.parameters, error) ||
        !readTexture(input, *findSection(sections, SectionKind::Transmittance),
                     loaded.transmittance, error) ||
        !readTexture(input, *findSection(sections, SectionKind::Scattering),
                     loaded.scattering, error) ||
        !readTexture(input, *findSection(sections, SectionKind::Irradiance),
                     loaded.irradiance, error))
        return false;
    if (!combined &&
        !readTexture(input, *findSection(sections, SectionKind::SingleMie),
                     loaded.singleMie, error))
        return false;
    if (!validateData(loaded, error))
        return false;

    data = std::move(loaded);
    return true;
}

} // namespace celestia::engine
