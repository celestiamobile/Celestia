// SPDX-FileCopyrightText: German Aerospace Center (DLR) <cosmoscout@dlr.de>
// SPDX-FileCopyrightText: 2017 Eric Bruneton
// SPDX-FileCopyrightText: 2008 INRIA
// SPDX-FileCopyrightText: 2026 Celestia Development Team
// SPDX-License-Identifier: BSD-3-Clause
//
// Runtime lookup model adapted from CosmoScout VR's Bruneton atmosphere
// implementation:
// https://github.com/cosmoscout/cosmoscout-vr/tree/main/plugins/csp-atmospheres

in vec3 brunetonPosition;

#ifdef GL_ES
precision highp sampler2D;
precision highp sampler3D;
#endif

uniform sampler2D uPhaseTexture;
uniform sampler2D uTransmittanceTexture;
uniform sampler2D uIrradianceTexture;
uniform sampler3D uMultipleScatteringTexture;
uniform sampler3D uSingleAerosolsScatteringTexture;
uniform sampler2D uThetaDeviationTexture;

uniform vec3 uCamera;
uniform vec3 uSunDirection;
uniform vec3 uSolarIlluminance;
uniform float uBottomRadius;
uniform float uTopRadius;
uniform float uSunAngularRadius;
uniform float uMuSMin;
uniform int uTransmittanceTextureWidth;
uniform int uTransmittanceTextureHeight;
uniform int uScatteringTextureRSize;
uniform int uScatteringTextureMuSize;
uniform int uScatteringTextureMuSSize;
uniform int uScatteringTextureNuSize;
uniform int uIrradianceTextureWidth;
uniform int uIrradianceTextureHeight;
uniform int uRefraction;
uniform int uHasSun;
uniform int uRenderMode;

const float PI = 3.14159265358979323846;

vec4 sampleLut(sampler2D lut, vec2 uv)
{
#ifdef GL_ES
    ivec2 size = textureSize(lut, 0);
    vec2 position = uv * vec2(size) - 0.5;
    ivec2 lower = ivec2(floor(position));
    ivec2 upper = lower + ivec2(1);
    vec2 interpolation = fract(position);
    lower = clamp(lower, ivec2(0), size - ivec2(1));
    upper = clamp(upper, ivec2(0), size - ivec2(1));
    vec4 lowerRow = mix(texelFetch(lut, lower, 0),
                        texelFetch(lut, ivec2(upper.x, lower.y), 0),
                        interpolation.x);
    vec4 upperRow = mix(texelFetch(lut, ivec2(lower.x, upper.y), 0),
                        texelFetch(lut, upper, 0),
                        interpolation.x);
    return mix(lowerRow, upperRow, interpolation.y);
#else
    return texture(lut, uv);
#endif
}

vec4 sampleLut(sampler3D lut, vec3 uvw)
{
#ifdef GL_ES
    ivec3 size = textureSize(lut, 0);
    vec3 position = uvw * vec3(size) - 0.5;
    ivec3 lower = ivec3(floor(position));
    ivec3 upper = lower + ivec3(1);
    vec3 interpolation = fract(position);
    lower = clamp(lower, ivec3(0), size - ivec3(1));
    upper = clamp(upper, ivec3(0), size - ivec3(1));

    vec4 z0y0 = mix(texelFetch(lut, lower, 0),
                    texelFetch(lut, ivec3(upper.x, lower.y, lower.z), 0),
                    interpolation.x);
    vec4 z0y1 = mix(texelFetch(lut, ivec3(lower.x, upper.y, lower.z), 0),
                    texelFetch(lut, ivec3(upper.x, upper.y, lower.z), 0),
                    interpolation.x);
    vec4 z1y0 = mix(texelFetch(lut, ivec3(lower.x, lower.y, upper.z), 0),
                    texelFetch(lut, ivec3(upper.x, lower.y, upper.z), 0),
                    interpolation.x);
    vec4 z1y1 = mix(texelFetch(lut, ivec3(lower.x, upper.y, upper.z), 0),
                    texelFetch(lut, upper, 0),
                    interpolation.x);
    return mix(mix(z0y0, z0y1, interpolation.y),
               mix(z1y0, z1y1, interpolation.y),
               interpolation.z);
#else
    return texture(lut, uvw);
#endif
}

float clampCosine(float mu)
{
    return clamp(mu, -1.0, 1.0);
}

float clampDistance(float distance)
{
    return max(distance, 0.0);
}

float clampRadius(float radius)
{
    return clamp(radius, uBottomRadius, uTopRadius);
}

float safeSqrt(float value)
{
    return sqrt(max(value, 0.0));
}

float distanceToTopAtmosphereBoundary(float radius, float mu)
{
    float discriminant =
        radius * radius * (mu * mu - 1.0) + uTopRadius * uTopRadius;
    return clampDistance(-radius * mu + safeSqrt(discriminant));
}

float distanceToBottomAtmosphereBoundary(float radius, float mu)
{
    float discriminant =
        radius * radius * (mu * mu - 1.0) + uBottomRadius * uBottomRadius;
    return clampDistance(-radius * mu - safeSqrt(discriminant));
}

bool rayIntersectsGround(float radius, float mu)
{
    return mu < 0.0 &&
           radius * radius * (mu * mu - 1.0) +
                   uBottomRadius * uBottomRadius >=
               0.0;
}

float textureCoordFromUnitRange(float value, int textureSize)
{
    return 0.5 / float(textureSize) +
           value * (1.0 - 1.0 / float(textureSize));
}

vec2 transmittanceTextureUvFromRMu(float radius, float mu)
{
    float horizonDistance =
        sqrt(uTopRadius * uTopRadius - uBottomRadius * uBottomRadius);
    float rho = safeSqrt(radius * radius - uBottomRadius * uBottomRadius);
    float distance = distanceToTopAtmosphereBoundary(radius, mu);
    float minimumDistance = uTopRadius - radius;
    float maximumDistance = rho + horizonDistance;
    float xMu = (distance - minimumDistance) /
                (maximumDistance - minimumDistance);
    float xRadius = rho / horizonDistance;
    return vec2(
        textureCoordFromUnitRange(xMu, uTransmittanceTextureWidth),
        textureCoordFromUnitRange(xRadius, uTransmittanceTextureHeight));
}

vec3 transmittanceToTopAtmosphereBoundary(float radius, float mu)
{
    return sampleLut(uTransmittanceTexture,
                     transmittanceTextureUvFromRMu(radius, mu))
        .rgb;
}

vec3 transmittanceBetween(float radius,
                          float mu,
                          float distance,
                          bool intersectsGround)
{
    float destinationRadius = clampRadius(sqrt(
        distance * distance + 2.0 * radius * mu * distance + radius * radius));
    float destinationMu =
        clampCosine((radius * mu + distance) / destinationRadius);

    if (intersectsGround)
    {
        return min(
            transmittanceToTopAtmosphereBoundary(destinationRadius, -destinationMu) /
                transmittanceToTopAtmosphereBoundary(radius, -mu),
            vec3(1.0));
    }

    return min(
        transmittanceToTopAtmosphereBoundary(radius, mu) /
            transmittanceToTopAtmosphereBoundary(destinationRadius, destinationMu),
        vec3(1.0));
}

vec3 transmittanceToSun(float radius, float muSun)
{
    float sinHorizon = uBottomRadius / radius;
    float cosHorizon = -sqrt(max(1.0 - sinHorizon * sinHorizon, 0.0));
    return transmittanceToTopAtmosphereBoundary(radius, muSun) *
           smoothstep(-sinHorizon * uSunAngularRadius,
                      sinHorizon * uSunAngularRadius,
                      muSun - cosHorizon);
}

vec4 scatteringTextureUvwz(float radius,
                           float mu,
                           float muSun,
                           float nu,
                           bool intersectsGround)
{
    float horizonDistance =
        sqrt(uTopRadius * uTopRadius - uBottomRadius * uBottomRadius);
    float rho = safeSqrt(radius * radius - uBottomRadius * uBottomRadius);
    float uRadius = textureCoordFromUnitRange(
        rho / horizonDistance, uScatteringTextureRSize);

    float radiusMu = radius * mu;
    float discriminant =
        radiusMu * radiusMu - radius * radius + uBottomRadius * uBottomRadius;
    float uMu;
    if (intersectsGround)
    {
        float distance = -radiusMu - safeSqrt(discriminant);
        float minimumDistance = radius - uBottomRadius;
        float maximumDistance = rho;
        float value = maximumDistance == minimumDistance
            ? 0.0
            : (distance - minimumDistance) /
                  (maximumDistance - minimumDistance);
        uMu = 0.5 -
              0.5 * textureCoordFromUnitRange(
                        value, uScatteringTextureMuSize / 2);
    }
    else
    {
        float distance =
            -radiusMu + safeSqrt(discriminant + horizonDistance * horizonDistance);
        float minimumDistance = uTopRadius - radius;
        float maximumDistance = rho + horizonDistance;
        uMu = 0.5 +
              0.5 * textureCoordFromUnitRange(
                        (distance - minimumDistance) /
                            (maximumDistance - minimumDistance),
                        uScatteringTextureMuSize / 2);
    }

    float distance =
        distanceToTopAtmosphereBoundary(uBottomRadius, muSun);
    float minimumDistance = uTopRadius - uBottomRadius;
    float maximumDistance = horizonDistance;
    float a = (distance - minimumDistance) /
              (maximumDistance - minimumDistance);
    float limitDistance =
        distanceToTopAtmosphereBoundary(uBottomRadius, uMuSMin);
    float limit = (limitDistance - minimumDistance) /
                  (maximumDistance - minimumDistance);
    float uMuSun = textureCoordFromUnitRange(
        max(1.0 - a / limit, 0.0) / (1.0 + a),
        uScatteringTextureMuSSize);

    return vec4((nu + 1.0) * 0.5, uMuSun, uMu, uRadius);
}

vec2 irradianceTextureUv(float radius, float muSun)
{
    float xRadius =
        (radius - uBottomRadius) / (uTopRadius - uBottomRadius);
    float xMuSun = muSun * 0.5 + 0.5;
    return vec2(
        textureCoordFromUnitRange(xMuSun, uIrradianceTextureWidth),
        textureCoordFromUnitRange(xRadius, uIrradianceTextureHeight));
}

vec3 moleculePhaseFunction(float nu)
{
    return sampleLut(uPhaseTexture,
                     vec2(acos(clampCosine(nu)) / PI, 0.0))
        .rgb;
}

vec3 aerosolPhaseFunction(float nu)
{
    return sampleLut(uPhaseTexture,
                     vec2(acos(clampCosine(nu)) / PI, 1.0))
        .rgb;
}

void combinedScattering(float radius,
                        float mu,
                        float muSun,
                        float nu,
                        bool intersectsGround,
                        out vec3 multipleScattering,
                        out vec3 singleAerosolsScattering)
{
    vec4 uvwz =
        scatteringTextureUvwz(radius, mu, muSun, nu, intersectsGround);
    float textureX = uvwz.x * float(uScatteringTextureNuSize - 1);
    float slice = floor(textureX);
    float interpolation = textureX - slice;
    vec3 uvw0 = vec3(
        (slice + uvwz.y) / float(uScatteringTextureNuSize), uvwz.z, uvwz.w);
    vec3 uvw1 = vec3(
        (slice + 1.0 + uvwz.y) / float(uScatteringTextureNuSize),
        uvwz.z,
        uvwz.w);

    multipleScattering =
        sampleLut(uMultipleScatteringTexture, uvw0).rgb *
            (1.0 - interpolation) +
        sampleLut(uMultipleScatteringTexture, uvw1).rgb * interpolation;
    singleAerosolsScattering =
        sampleLut(uSingleAerosolsScatteringTexture, uvw0).rgb *
            (1.0 - interpolation) +
        sampleLut(uSingleAerosolsScatteringTexture, uvw1).rgb * interpolation;
}

vec3 skyLuminance(vec3 camera,
                  vec3 viewRay,
                  vec3 sunDirection,
                  out vec3 transmittance)
{
    float radius = length(camera);
    float radiusMu = dot(camera, viewRay);
    float discriminant = radiusMu * radiusMu - radius * radius +
                         uTopRadius * uTopRadius;
    float distanceToTop = -radiusMu - safeSqrt(discriminant);
    if (distanceToTop > 0.0)
    {
        camera += viewRay * distanceToTop;
        radius = uTopRadius;
        radiusMu += distanceToTop;
    }
    else if (radius > uTopRadius)
    {
        transmittance = vec3(1.0);
        return vec3(0.0);
    }

    float mu = radiusMu / radius;
    float muSun = dot(camera, sunDirection) / radius;
    float nu = dot(viewRay, sunDirection);
    bool intersectsGround = rayIntersectsGround(radius, mu);
    transmittance = intersectsGround
        ? vec3(0.0)
        : transmittanceToTopAtmosphereBoundary(radius, mu);

    vec3 multipleScattering;
    vec3 singleAerosolsScattering;
    combinedScattering(radius,
                       mu,
                       muSun,
                       nu,
                       intersectsGround,
                       multipleScattering,
                       singleAerosolsScattering);
    return multipleScattering * moleculePhaseFunction(nu) +
           singleAerosolsScattering * aerosolPhaseFunction(nu);
}

vec3 skyLuminanceToPoint(vec3 camera,
                         vec3 point,
                         vec3 sunDirection,
                         out vec3 transmittance)
{
    vec3 viewRay = normalize(point - camera);
    float radius = length(camera);
    float radiusMu = dot(camera, viewRay);
    float distanceToTop =
        -radiusMu -
        safeSqrt(radiusMu * radiusMu - radius * radius +
                 uTopRadius * uTopRadius);
    if (distanceToTop > 0.0)
    {
        camera += viewRay * distanceToTop;
        radius = uTopRadius;
        radiusMu += distanceToTop;
    }

    float mu = radiusMu / radius;
    float muSun = dot(camera, sunDirection) / radius;
    float nu = dot(viewRay, sunDirection);
    float distance = length(point - camera);
    bool intersectsGround = rayIntersectsGround(radius, mu);
    transmittance =
        transmittanceBetween(radius, mu, distance, mu < 0.0);

    vec3 multipleScattering;
    vec3 singleAerosolsScattering;
    combinedScattering(radius,
                       mu,
                       muSun,
                       nu,
                       intersectsGround,
                       multipleScattering,
                       singleAerosolsScattering);

    float pointRadius = clampRadius(sqrt(
        distance * distance + 2.0 * radius * mu * distance + radius * radius));
    float pointMu = (radius * mu + distance) / pointRadius;
    float pointMuSun = (radius * muSun + distance * nu) / pointRadius;
    vec3 pointMultipleScattering;
    vec3 pointSingleAerosolsScattering;
    combinedScattering(pointRadius,
                       pointMu,
                       pointMuSun,
                       nu,
                       intersectsGround,
                       pointMultipleScattering,
                       pointSingleAerosolsScattering);

    multipleScattering =
        max(multipleScattering - transmittance * pointMultipleScattering,
            vec3(0.0));
    singleAerosolsScattering =
        max(singleAerosolsScattering -
                transmittance * pointSingleAerosolsScattering,
            vec3(0.0));
    return multipleScattering * moleculePhaseFunction(nu) +
           singleAerosolsScattering * aerosolPhaseFunction(nu);
}

vec3 sunAndSkyIlluminance(vec3 point,
                          vec3 sunDirection,
                          out vec3 skyIlluminance)
{
    float radius = length(point);
    float muSun = dot(point, sunDirection) / radius;
    skyIlluminance =
        sampleLut(uIrradianceTexture, irradianceTextureUv(radius, muSun)).rgb;
    return uSolarIlluminance * transmittanceToSun(radius, muSun);
}

vec3 refractedRay(vec3 camera, vec3 ray, out bool hitsGround)
{
    if (uRefraction == 0)
    {
        hitsGround = false;
        return ray;
    }

    float radius = length(camera);
    float mu = dot(camera / radius, ray);
    vec2 deviationAndContactRadius =
        sampleLut(uThetaDeviationTexture,
                  transmittanceTextureUvFromRMu(radius, mu))
            .rg;
    hitsGround = deviationAndContactRadius.y < 0.0;

    vec3 axis = cross(camera, ray);
    float axisLength = length(axis);
    if (axisLength == 0.0)
        return ray;
    axis /= axisLength;

    float sine = sin(deviationAndContactRadius.x);
    float cosine = sqrt(max(1.0 - sine * sine, 0.0));
    return ray * cosine + cross(axis, ray) * sine +
           axis * dot(axis, ray) * (1.0 - cosine);
}

vec2 intersectSphere(vec3 origin, vec3 direction, float radius)
{
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0)
        return vec2(1.0, -1.0);
    float root = sqrt(discriminant);
    return vec2(-b - root, -b + root);
}

void main(void)
{
    vec3 shellPoint = brunetonPosition * uTopRadius;
    vec3 viewRay = normalize(shellPoint - uCamera);
    bool refractedIntoGround;
    viewRay = refractedRay(uCamera, viewRay, refractedIntoGround);

    vec2 groundIntersections =
        intersectSphere(uCamera, viewRay, uBottomRadius);
    float groundDistance = groundIntersections.x > 0.0
        ? groundIntersections.x
        : -1.0;

    vec3 transmittance;
    vec3 inScatter;
    vec3 surfaceFactor = vec3(1.0);
    if (groundDistance > 0.0 || refractedIntoGround)
    {
        float distance = groundDistance > 0.0
            ? groundDistance
            : distanceToBottomAtmosphereBoundary(
                  length(uCamera), dot(normalize(uCamera), viewRay));
        vec3 surfacePoint = uCamera + viewRay * distance;
        inScatter = skyLuminanceToPoint(
            uCamera, surfacePoint, uSunDirection, transmittance);
        if (uHasSun != 0)
        {
            vec3 skyIlluminance;
            vec3 sunIlluminance = sunAndSkyIlluminance(
                surfacePoint, uSunDirection, skyIlluminance);
            surfaceFactor =
                (sunIlluminance + skyIlluminance) / uSolarIlluminance;
        }
    }
    else
    {
        inScatter =
            skyLuminance(uCamera, viewRay, uSunDirection, transmittance);
    }

    if (uRenderMode == 0)
    {
        fragColor = vec4(clamp(transmittance * surfaceFactor, 0.0, 1.0), 1.0);
    }
    else
    {
        float referenceIlluminance = max(
            max(uSolarIlluminance.r, uSolarIlluminance.g),
            uSolarIlluminance.b);
        fragColor = vec4(max(inScatter / referenceIlluminance, vec3(0.0)), 1.0);
    }
}
