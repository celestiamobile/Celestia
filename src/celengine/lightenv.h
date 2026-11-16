// lightenv.h
//
// Structures that describe the lighting environment for rendering objects
// in Celestia.
//
// Copyright (C) 2006, Chris Laurel <claurel@shatters.net>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <array>
#include <vector>

#include <celutil/color.h>
#include <Eigen/Core>
#include <Eigen/Geometry>

constexpr inline unsigned int MaxLights = 8;

class Body;
struct RingSystem;

struct DirectionalLight
{
    Color color;
    float irradiance{ 0.0f };
    Eigen::Vector3f direction_eye{ Eigen::Vector3f::UnitZ() };
    Eigen::Vector3f direction_obj{ Eigen::Vector3f::UnitZ() };

    // Required for eclipse shadows only--may be able to use
    // distance instead of position.
    Eigen::Vector3d position{ Eigen::Vector3d::Zero() };  // position relative to the lit object
    float apparentSize{ 0.0f };
    bool castsShadows{ false };
};

struct EclipseShadow
{
    const Body* caster{ nullptr };
    Eigen::Quaternionf casterOrientation{ Eigen::Quaternionf::Identity() };
    Eigen::Vector3f origin{ Eigen::Vector3f::Zero() };
    Eigen::Vector3f direction{ Eigen::Vector3f::UnitZ() };
    float penumbraRadius{ 0.0f };
    float umbraRadius{ 0.0f };
    float maxDepth{ 0.0f };
};

struct RingShadow
{
    const RingSystem* ringSystem{ nullptr };
    Eigen::Quaternionf casterOrientation{ Eigen::Quaternionf::Identity() };
    Eigen::Vector3f origin{ Eigen::Vector3f::Zero() };
    Eigen::Vector3f direction{ Eigen::Vector3f::UnitZ() };
    float texLod{ 0.0f };
};

class LightingState
{
public:
    using EclipseShadowVector = std::vector<EclipseShadow>;

    LightingState()
    {
        shadows.fill(nullptr);
    };

    unsigned int nLights{ 0 };
    std::array<DirectionalLight, MaxLights> lights;
    std::array<EclipseShadowVector*, MaxLights> shadows;
    std::array<RingShadow, MaxLights> ringShadows;
    const RingSystem* shadowingRingSystem{ nullptr }; // nullptr when there are no ring shadows
    Eigen::Vector3f ringPlaneNormal{ Eigen::Vector3f::UnitZ() };
    Eigen::Vector3f ringCenter{ Eigen::Vector3f::Zero() };

    Eigen::Vector3f eyeDir_obj{ -Eigen::Vector3f::UnitZ() };
    Eigen::Vector3f eyePos_obj{ -Eigen::Vector3f::UnitZ() };

    Eigen::Vector3f ambientColor{ Eigen::Vector3f::Zero() };
};
