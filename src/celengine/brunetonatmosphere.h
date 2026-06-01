// brunetonatmosphere.h
//
// Copyright (C) 2026, the Celestia Development Team
//
// C++ mirror of the std140 `AtmosphereBlock` UBO declared in
// `shaders/atmosphere_frag.glsl`. The shader is shared across every
// atmospheric body; per-body parameters live in this block plus the
// three precomputed LUTs referenced via sampler uniforms.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <cstdint>

namespace celestia::engine
{

// std140 layout: scalars 4B, vec3 has 16B alignment with 12B size, arrays
// of any type have a vec4-aligned stride, structs are aligned/padded to
// vec4. Members below are laid out to match the GLSL struct declarations
// in atmosphere_frag.glsl. Do not reorder without updating the shader.

struct alignas(16) Std140DensityProfileLayer
{
    float width{ 0.0f };
    float exp_term{ 0.0f };
    float exp_scale{ 0.0f };
    float linear_term{ 0.0f };
    float constant_term{ 0.0f };
    float _pad0[3]{};
};
static_assert(sizeof(Std140DensityProfileLayer) == 32,
              "DensityProfileLayer must be 32 bytes under std140");

struct alignas(16) Std140DensityProfile
{
    Std140DensityProfileLayer layers[2]{};
};
static_assert(sizeof(Std140DensityProfile) == 64,
              "DensityProfile must be 64 bytes under std140");

struct alignas(16) Std140AtmosphereParameters
{
    // solar_irradiance.xyz + sun_angular_radius packed in trailing slot.
    float    solar_irradiance[3]{ 0.0f, 0.0f, 0.0f };  // offset   0
    float    sun_angular_radius{ 0.0f };               // offset  12
    float    bottom_radius{ 0.0f };                    // offset  16
    float    top_radius{ 0.0f };                       // offset  20
    float    _pad0[2]{};                               // offset  24..32

    Std140DensityProfile rayleigh_density{};           // offset  32..96
    float    rayleigh_scattering[3]{ 0, 0, 0 };        // offset  96
    float    _pad1{ 0.0f };                            // offset 108..112

    Std140DensityProfile mie_density{};                // offset 112..176
    float    mie_scattering[3]{ 0, 0, 0 };             // offset 176
    float    _pad2{ 0.0f };                            // offset 188..192
    float    mie_extinction[3]{ 0, 0, 0 };             // offset 192
    float    mie_phase_function_g{ 0.0f };             // offset 204

    Std140DensityProfile absorption_density{};         // offset 208..272
    float    absorption_extinction[3]{ 0, 0, 0 };      // offset 272
    float    _pad3{ 0.0f };                            // offset 284..288
    float    ground_albedo[3]{ 0, 0, 0 };              // offset 288
    float    mu_s_min{ 0.0f };                         // offset 300
};
static_assert(sizeof(Std140AtmosphereParameters) == 304,
              "AtmosphereParameters must be 304 bytes under std140");

struct alignas(16) Std140AtmosphereBlock
{
    Std140AtmosphereParameters ATMOSPHERE{};                 // offset   0..304
    float SKY_SPECTRAL_RADIANCE_TO_LUMINANCE[3]{ 0, 0, 0 };  // offset 304
    float _pad0{ 0.0f };                                     // offset 316..320
    float SUN_SPECTRAL_RADIANCE_TO_LUMINANCE[3]{ 0, 0, 0 };  // offset 320
    float _pad1{ 0.0f };                                     // offset 332..336
};
static_assert(sizeof(Std140AtmosphereBlock) == 336,
              "AtmosphereBlock must be 336 bytes under std140");

} // namespace celestia::engine
