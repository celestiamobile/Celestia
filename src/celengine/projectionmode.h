// projectionmode.h
//
// Copyright (C) 2023-present, Celestia Development Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <memory>
#include <Eigen/Core>

namespace celestia::engine
{

constexpr float standardFOV = 45.0f;

class ProjectionMode
{
public:
    virtual ~ProjectionMode() = default;

    virtual Eigen::Matrix4f getProjectionMatrix(float nearZ, float farZ, float zoom) const = 0;
    virtual float getMinimumFOV() const = 0;
    virtual float getMaximumFOV() const = 0;
    virtual float getFOV(float zoom) const = 0;
    virtual float getZoom(float fov) const = 0;
    virtual float getPixelSize(float zoom) const = 0;
    virtual float getFieldCorrection(float zoom) const = 0;

    // Calculate the cosine of half the maximum field of view. We'll use this for
    // fast testing of object visibility.
    virtual double getViewConeAngleMax(float zoom) const = 0;

    virtual float getNormalizedDeviceZ(float nearZ, float farZ, float z) const = 0;

    virtual Eigen::Vector3f getPickRay(float x, float y, float zoom) const = 0;
    virtual bool isFisheye() const = 0;

    virtual void setScreenDpi(int screenDpi) = 0;
    virtual void setDistanceToScreen(int distanceToScreen) = 0;
    virtual void setSize(float width, float height) = 0;

    virtual std::unique_ptr<ProjectionMode> clone() = 0;
};

}
