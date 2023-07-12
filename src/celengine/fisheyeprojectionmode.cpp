// fisheyeprojectionmode.cpp
//
// Copyright (C) 2023-present, Celestia Development Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "fisheyeprojectionmode.h"
#include <celmath/geomutil.h>
#include <celengine/shadermanager.h>

namespace celestia::engine
{

constexpr float fisheyeFOV = celmath::degToRad(179.99f);

FisheyeProjectionMode::FisheyeProjectionMode(float width, float height, int screenDpi) :
    ProjectionMode(width, height, 0, screenDpi)
{
}

Eigen::Matrix4f FisheyeProjectionMode::getProjectionMatrix(float nearZ, float farZ, float /*zoom*/) const
{
    float aspectRatio = width / height;
    return celmath::Ortho(-aspectRatio, aspectRatio, -1.0f, 1.0f, nearZ, farZ);
}

float FisheyeProjectionMode::getMinimumFOV() const
{
    return fisheyeFOV;
}

float FisheyeProjectionMode::getMaximumFOV() const
{
    return fisheyeFOV;
}

float FisheyeProjectionMode::getFOV(float /*zoom*/) const
{
    return fisheyeFOV;
}

float FisheyeProjectionMode::getZoom(float /*fov*/) const
{
    return 1.0f;
}

float FisheyeProjectionMode::getPixelSize(float /*zoom*/) const
{
    return 2.0f / height;
}

float FisheyeProjectionMode::getFieldCorrection(float /*zoom*/) const
{
    return 2.0f - 2000.0f / (height / (static_cast<float>(screenDpi) / 25.4f / 3.78f) + 1000.0f); // larger window height = more stars to display
}

double FisheyeProjectionMode::getViewConeAngleMax(float /*zoom*/) const
{
    return std::cos(static_cast<double>(fisheyeFOV) / 2.0);
}

float FisheyeProjectionMode::getNormalizedDeviceZ(float nearZ, float farZ, float z) const
{
    // just apply a linear transformation since fisheye uses
    // orthographic projection already.
    float d0 = farZ - nearZ;
    return 1.0f - (z - nearZ) / d0 * 2.0f;
}

Eigen::Vector3f FisheyeProjectionMode::getPickRay(float x, float y, float /*zoom*/) const
{
    float r = std::hypot(x, y);
    float phi = celestia::numbers::pi_v<float> * r;
    float sin_phi = std::sin(phi);
    float theta = std::atan2(y, x);
    float newX = sin_phi * std::cos(theta);
    float newY = sin_phi * std::sin(theta);
    float newZ = std::cos(phi);
    Eigen::Vector3f pickDirection(newX, newY, -newZ);

    return pickDirection;
}

Eigen::Vector2f FisheyeProjectionMode::getRayIntersection(Eigen::Vector3f pickRay, float /*zoom*/) const
{
    float phi = std::acos(pickRay.z());
    float r = phi / celestia::numbers::pi_v<float>;
    float theta = std::atan2(pickRay.y(), pickRay.x());
    float x = r * std::cos(theta);
    float y = r * std::sin(theta);
    return Eigen::Vector2f(x, y);
}

void FisheyeProjectionMode::configureShaderManager(ShaderManager *shaderManager) const
{
    shaderManager->setFisheyeEnabled(true);
}

bool FisheyeProjectionMode::project(const Eigen::Vector3f& pos, const Eigen::Matrix4f existingModelViewMatrix, const Eigen::Matrix4f existingProjectionMatrix, const Eigen::Matrix4f /*existingMVPMatrix*/, const int viewport[4], Eigen::Vector3f& result) const
{
    return celmath::ProjectFisheye(pos, existingModelViewMatrix, existingProjectionMatrix, viewport, result);
}

}
