// multiplescatteringlut.cpp
//
// Copyright (C) 2026, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "multiplescatteringlut.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <future>
#include <utility>
#include <vector>

#include <Eigen/Core>

#include <celcompat/numbers.h>
#include <celengine/atmosphere.h>
#include <celengine/glsupport.h>

namespace celestia::render
{

namespace
{

constexpr int LutSize = 32;
constexpr int DirectionGridSize = 8;
constexpr int RayStepCount = 20;
constexpr int TransmittanceMuSize = 128;
constexpr int TransmittanceHeightSize = 64;
constexpr int TransmittanceStepCount = 64;
constexpr std::size_t MaxCacheEntries = 8;

using Spectrum = Eigen::Vector3f;

struct Parameters
{
    float planetRadius;
    float atmosphereHeight;
    float scaleHeight;
    float mieCoeff;
    bool normalizedPhaseFunctions;
    Spectrum rayleighCoeff;
    Spectrum absorptionCoeff;

    bool operator==(const Parameters& other) const
    {
        return planetRadius == other.planetRadius &&
               atmosphereHeight == other.atmosphereHeight &&
               scaleHeight == other.scaleHeight &&
               mieCoeff == other.mieCoeff &&
               normalizedPhaseFunctions == other.normalizedPhaseFunctions &&
               rayleighCoeff == other.rayleighCoeff &&
               absorptionCoeff == other.absorptionCoeff;
    }
};

struct RaySpan
{
    float distance;
    bool hitsGround;
};

RaySpan
traceAtmosphere(const Spectrum& origin,
                const Spectrum& direction,
                float bottomRadius,
                float topRadius)
{
    const float projection = origin.dot(direction);
    const float radiusSquared = origin.squaredNorm();
    if (radiusSquared <= bottomRadius * bottomRadius && projection < 0.0f)
        return { 0.0f, true };

    const float topDiscriminant =
        projection * projection - radiusSquared + topRadius * topRadius;
    const float topDistance =
        -projection + std::sqrt(std::max(0.0f, topDiscriminant));

    const float bottomDiscriminant =
        projection * projection - radiusSquared + bottomRadius * bottomRadius;
    if (bottomDiscriminant <= 0.0f)
        return { topDistance, false };

    const float root = std::sqrt(bottomDiscriminant);
    const float nearDistance = -projection - root;
    const float farDistance = -projection + root;
    const float groundDistance = nearDistance > 0.0f ? nearDistance : farDistance;
    if (groundDistance > 0.0f && groundDistance < topDistance)
        return { groundDistance, true };

    return { topDistance, false };
}

float
densityAt(const Spectrum& point, const Parameters& parameters)
{
    const float altitude = std::max(0.0f, point.norm() - parameters.planetRadius);
    return std::exp(-altitude / parameters.scaleHeight);
}

struct TransmittanceTable
{
    std::vector<Spectrum> values;
};

Spectrum
integrateTransmittance(const Spectrum& origin,
                       const Spectrum& direction,
                       const Parameters& parameters,
                       const Spectrum& extinction)
{
    const float topRadius = parameters.planetRadius + parameters.atmosphereHeight;
    const RaySpan ray = traceAtmosphere(origin, direction,
                                        parameters.planetRadius, topRadius);
    if (ray.hitsGround)
        return Spectrum::Zero();

    const float stepLength = ray.distance / static_cast<float>(TransmittanceStepCount);
    float opticalDepth = 0.0f;
    for (int i = 0; i < TransmittanceStepCount; ++i)
    {
        const float distance = (static_cast<float>(i) + 0.5f) * stepLength;
        opticalDepth += densityAt(origin + direction * distance, parameters) * stepLength;
    }

    return (-extinction * opticalDepth).array().exp().matrix();
}

TransmittanceTable
buildTransmittanceTable(const Parameters& parameters, const Spectrum& extinction)
{
    TransmittanceTable table;
    table.values.resize(TransmittanceMuSize * TransmittanceHeightSize);

    for (int y = 0; y < TransmittanceHeightSize; ++y)
    {
        const float heightFraction =
            static_cast<float>(y) / static_cast<float>(TransmittanceHeightSize - 1);
        const float radius =
            parameters.planetRadius + heightFraction * parameters.atmosphereHeight;
        const Spectrum origin(0.0f, 0.0f, radius);

        for (int x = 0; x < TransmittanceMuSize; ++x)
        {
            const float mu = -1.0f + 2.0f * static_cast<float>(x) /
                                         static_cast<float>(TransmittanceMuSize - 1);
            const float horizontal = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
            const Spectrum direction(horizontal, 0.0f, mu);
            table.values[y * TransmittanceMuSize + x] =
                integrateTransmittance(origin, direction, parameters, extinction);
        }
    }

    return table;
}

Spectrum
sampleTransmittance(const TransmittanceTable& table,
                    float radius,
                    float mu,
                    const Parameters& parameters)
{
    const float x = std::clamp((mu + 1.0f) * 0.5f, 0.0f, 1.0f) *
                    static_cast<float>(TransmittanceMuSize - 1);
    const float y = std::clamp((radius - parameters.planetRadius) /
                                   parameters.atmosphereHeight,
                               0.0f, 1.0f) *
                    static_cast<float>(TransmittanceHeightSize - 1);

    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, TransmittanceMuSize - 1);
    const int y1 = std::min(y0 + 1, TransmittanceHeightSize - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const Spectrum c0 = (1.0f - tx) * table.values[y0 * TransmittanceMuSize + x0] +
                        tx * table.values[y0 * TransmittanceMuSize + x1];
    const Spectrum c1 = (1.0f - tx) * table.values[y1 * TransmittanceMuSize + x0] +
                        tx * table.values[y1 * TransmittanceMuSize + x1];
    return (1.0f - ty) * c0 + ty * c1;
}

struct DirectionIntegral
{
    Spectrum firstOrder{ Spectrum::Zero() };
    Spectrum transfer{ Spectrum::Zero() };
};

DirectionIntegral
integrateDirection(const Spectrum& origin,
                   const Spectrum& direction,
                   const Spectrum& sunDirection,
                   const Parameters& parameters,
                   const Spectrum& scattering,
                   const Spectrum& extinction,
                   const TransmittanceTable& transmittance)
{
    const float topRadius = parameters.planetRadius + parameters.atmosphereHeight;
    const RaySpan ray = traceAtmosphere(origin, direction,
                                        parameters.planetRadius, topRadius);
    const float stepLength = ray.distance / static_cast<float>(RayStepCount);

    DirectionIntegral result;
    Spectrum throughput = Spectrum::Ones();
    for (int i = 0; i < RayStepCount; ++i)
    {
        const float distance = (static_cast<float>(i) + 0.5f) * stepLength;
        const Spectrum point = origin + direction * distance;
        const float density = densityAt(point, parameters);
        const Spectrum mediumExtinction = extinction * density;
        const Spectrum segmentTransmittance =
            (-mediumExtinction * stepLength).array().exp().matrix();
        const Spectrum scatterFraction =
            scattering.cwiseQuotient(extinction.cwiseMax(1.0e-18f))
                      .cwiseProduct(Spectrum::Ones() - segmentTransmittance);

        const float radius = point.norm();
        const float sunMu = point.dot(sunDirection) / radius;
        const Spectrum sunTransmittance =
            sampleTransmittance(transmittance, radius, sunMu, parameters);
        result.firstOrder += throughput.cwiseProduct(scatterFraction)
                                       .cwiseProduct(sunTransmittance) *
                             (parameters.normalizedPhaseFunctions
                                  ? 1.0f / (4.0f * celestia::numbers::pi_v<float>)
                                  : 1.0f);
        result.transfer += throughput.cwiseProduct(scatterFraction);
        throughput = throughput.cwiseProduct(segmentTransmittance);
    }

    return result;
}

std::vector<float>
generateLut(const Parameters& parameters)
{
    const Spectrum scattering =
        parameters.rayleighCoeff + Spectrum::Constant(parameters.mieCoeff);
    const Spectrum extinction = scattering + parameters.absorptionCoeff;
    const TransmittanceTable transmittance =
        buildTransmittanceTable(parameters, extinction);

    std::vector<float> pixels(LutSize * LutSize * 4, 0.0f);
    constexpr float directionCount =
        static_cast<float>(DirectionGridSize * DirectionGridSize);
    const float topRadius = parameters.planetRadius + parameters.atmosphereHeight;

    for (int y = 0; y < LutSize; ++y)
    {
        const float altitudeFraction =
            static_cast<float>(y) / static_cast<float>(LutSize - 1);
        const float radius =
            parameters.planetRadius + altitudeFraction * (topRadius - parameters.planetRadius);
        const Spectrum origin(0.0f, 0.0f, radius);

        for (int x = 0; x < LutSize; ++x)
        {
            const float sunMu =
                -1.0f + 2.0f * static_cast<float>(x) / static_cast<float>(LutSize - 1);
            const float sunHorizontal = std::sqrt(std::max(0.0f, 1.0f - sunMu * sunMu));
            const Spectrum sunDirection(sunHorizontal, 0.0f, sunMu);
            Spectrum firstOrder = Spectrum::Zero();
            Spectrum transfer = Spectrum::Zero();

            for (int directionIndex = 0;
                 directionIndex < DirectionGridSize * DirectionGridSize;
                 ++directionIndex)
            {
                const int i = directionIndex / DirectionGridSize;
                const int j = directionIndex % DirectionGridSize;
                const float u = (static_cast<float>(i) + 0.5f) /
                                static_cast<float>(DirectionGridSize);
                const float v = (static_cast<float>(j) + 0.5f) /
                                static_cast<float>(DirectionGridSize);
                const float theta = 2.0f * celestia::numbers::pi_v<float> * u;
                const float cosPhi = 1.0f - 2.0f * v;
                const float sinPhi = std::sqrt(std::max(0.0f, 1.0f - cosPhi * cosPhi));
                const Spectrum direction(std::cos(theta) * sinPhi,
                                         std::sin(theta) * sinPhi,
                                         cosPhi);
                const DirectionIntegral integral =
                    integrateDirection(origin, direction, sunDirection, parameters,
                                       scattering, extinction, transmittance);
                firstOrder += integral.firstOrder;
                transfer += integral.transfer;
            }

            firstOrder /= directionCount;
            transfer /= directionCount;
            const Spectrum multipleScattering =
                firstOrder.cwiseQuotient((Spectrum::Ones() - transfer)
                                             .cwiseMax(1.0e-4f));
            const std::size_t offset =
                static_cast<std::size_t>((y * LutSize + x) * 4);
            pixels[offset] = multipleScattering.x();
            pixels[offset + 1] = multipleScattering.y();
            pixels[offset + 2] = multipleScattering.z();
            pixels[offset + 3] = 1.0f;
        }
    }

    return pixels;
}

struct CacheEntry
{
    Parameters parameters;
    std::future<std::vector<float>> future;
    GLuint texture{ 0 };
    std::size_t lastUsed{ 0 };
};

} // end unnamed namespace

struct MultipleScatteringLut::Impl
{
    std::vector<CacheEntry> entries;
    std::size_t useCounter{ 0 };
};

MultipleScatteringLut::MultipleScatteringLut() :
    m_impl(std::make_unique<Impl>())
{
}

MultipleScatteringLut::~MultipleScatteringLut()
{
    for (const CacheEntry& entry : m_impl->entries)
    {
        if (entry.texture != 0)
            glDeleteTextures(1, &entry.texture);
    }
}

bool
MultipleScatteringLut::bind(const Atmosphere& atmosphere,
                            float planetRadius,
                            float atmosphereHeight,
                            unsigned int textureUnit)
{
    if (planetRadius <= 0.0f || atmosphereHeight <= 0.0f ||
        atmosphere.mieScaleHeight <= 0.0f)
    {
        return false;
    }

    const Parameters parameters{
        planetRadius,
        atmosphereHeight,
        atmosphere.mieScaleHeight,
        atmosphere.mieCoeff,
        atmosphere.normalizedPhaseFunctions,
        atmosphere.rayleighCoeff,
        atmosphere.absorptionCoeff,
    };

    auto entryIt = std::find_if(m_impl->entries.begin(), m_impl->entries.end(),
                                [&parameters](const CacheEntry& entry)
                                {
                                    return entry.parameters == parameters;
                                });
    const std::size_t useCounter = ++m_impl->useCounter;
    if (entryIt == m_impl->entries.end())
    {
        if (m_impl->entries.size() >= MaxCacheEntries)
        {
            auto evictIt = m_impl->entries.end();
            for (auto it = m_impl->entries.begin(); it != m_impl->entries.end(); ++it)
            {
                const bool ready =
                    it->texture != 0 ||
                    it->future.wait_for(std::chrono::seconds(0)) ==
                        std::future_status::ready;
                if (ready &&
                    (evictIt == m_impl->entries.end() ||
                     it->lastUsed < evictIt->lastUsed))
                {
                    evictIt = it;
                }
            }
            if (evictIt == m_impl->entries.end())
                return false;

            if (evictIt->texture != 0)
                glDeleteTextures(1, &evictIt->texture);
            m_impl->entries.erase(evictIt);
        }

        CacheEntry entry{
            parameters,
            std::async(std::launch::async,
                       [parameters]() { return generateLut(parameters); }),
            0,
            useCounter,
        };
        m_impl->entries.push_back(std::move(entry));
        return false;
    }

    entryIt->lastUsed = useCounter;
    if (entryIt->texture == 0)
    {
        if (entryIt->future.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
        {
            return false;
        }

        const std::vector<float> pixels = entryIt->future.get();
        glGenTextures(1, &entryIt->texture);
        glBindTexture(GL_TEXTURE_2D, entryIt->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, LutSize, LutSize, 0,
                     GL_RGBA, GL_FLOAT, pixels.data());
    }

    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, entryIt->texture);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

} // namespace celestia::render
