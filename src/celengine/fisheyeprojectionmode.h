// fisheyeprojectionmode.h
//
// Copyright (C) 2023-present, Celestia Development Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <celengine/projectionmode.h>

namespace celestia::engine
{

class FisheyeProjectionMode : public ProjectionMode
{
public:
    FisheyeProjectionMode(float width, float height, int screenDpi);

    FisheyeProjectionMode(const FisheyeProjectionMode &) = default;
    FisheyeProjectionMode(FisheyeProjectionMode &&) = default;
    FisheyeProjectionMode &operator=(const FisheyeProjectionMode &) = default;
    FisheyeProjectionMode &operator=(FisheyeProjectionMode &&) = default;
    ~FisheyeProjectionMode() override = default;

    Eigen::Matrix4f getProjectionMatrix(float nearZ, float farZ, float zoom) const override;
    float getMinimumFOV() const override;
    float getMaximumFOV() const override;
    float getFOV(float zoom) const override;
    float getZoom(float fov) const override;
    float getPixelSize(float zoom) const override;
    float getFieldCorrection(float zoom) const override;
    double getViewConeAngleMax(float zoom) const override;

    float getNormalizedDeviceZ(float nearZ, float farZ, float z) const override;

    Eigen::Vector3f getPickRay(float x, float y, float zoom) const override;

    bool isFisheye() const override;

    void setScreenDpi(int screenDpi) override;
    void setDistanceToScreen(int distanceToScreen) override;
    void setSize(float width, float height) override;

    std::unique_ptr<ProjectionMode> clone() override;

private:
    float width;
    float height;
    int screenDpi;
};

}
