// atmfile.h
//
// Copyright (C) 2026, the Celestia Development Team
//
// Binary on-disk format for a precomputed Bruneton atmosphere ("`.atm`"
// file). One file holds the four LUTs and the AtmosphereParameters that
// the offline bake tool generated; the runtime loads it, uploads the
// LUTs as GL textures, and copies the parameters into the UBO mirrored
// by Std140AtmosphereBlock (see brunetonatmosphere.h).
//
// All fields are little-endian. Texel format is `RGBA32F` for every LUT
// (matches Bruneton's precompute output). LUT dimensions are fixed at
// build time and must agree with the const ints in
// shaders/atmosphere_frag.glsl.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <cstdint>

#include "brunetonatmosphere.h"

namespace celestia::engine
{

// Must match the corresponding `const int` declarations in
// shaders/atmosphere_frag.glsl. Keep in sync.
inline constexpr std::uint32_t AtmTransmittanceTextureWidth    = 256;
inline constexpr std::uint32_t AtmTransmittanceTextureHeight   = 64;
inline constexpr std::uint32_t AtmScatteringTextureRSize       = 32;
inline constexpr std::uint32_t AtmScatteringTextureMuSize      = 128;
inline constexpr std::uint32_t AtmScatteringTextureMuSSize     = 32;
inline constexpr std::uint32_t AtmScatteringTextureNuSize      = 8;
inline constexpr std::uint32_t AtmIrradianceTextureWidth       = 64;
inline constexpr std::uint32_t AtmIrradianceTextureHeight      = 16;

// Width of the flattened 3-D scattering texture. The shader stores the
// 4-D (nu, mu_s, mu, r) table as a 3-D texture with mu_s and nu packed
// into a single horizontal axis (see definitions.glsl).
inline constexpr std::uint32_t AtmScatteringTextureWidth =
    AtmScatteringTextureNuSize * AtmScatteringTextureMuSSize;
inline constexpr std::uint32_t AtmScatteringTextureHeight = AtmScatteringTextureMuSize;
inline constexpr std::uint32_t AtmScatteringTextureDepth  = AtmScatteringTextureRSize;

// Magic = 'CATM' little-endian; bumped whenever the on-disk format
// breaks compatibility (struct layout change, new section, etc.).
inline constexpr std::uint32_t AtmFileMagic   = 0x4D544143u; // 'C','A','T','M'
inline constexpr std::uint32_t AtmFileVersion = 1u;

// One float == 4 bytes. RGBA32F texels:
inline constexpr std::uint32_t AtmTexelBytes = 4u * sizeof(float);

inline constexpr std::uint64_t AtmTransmittanceBytes =
    static_cast<std::uint64_t>(AtmTransmittanceTextureWidth) *
    AtmTransmittanceTextureHeight * AtmTexelBytes;

inline constexpr std::uint64_t AtmScatteringBytes =
    static_cast<std::uint64_t>(AtmScatteringTextureWidth) *
    AtmScatteringTextureHeight * AtmScatteringTextureDepth * AtmTexelBytes;

inline constexpr std::uint64_t AtmIrradianceBytes =
    static_cast<std::uint64_t>(AtmIrradianceTextureWidth) *
    AtmIrradianceTextureHeight * AtmTexelBytes;

// File layout:
//   [AtmFileHeader]
//   [transmittance LUT     : AtmTransmittanceBytes]
//   [scattering   LUT      : AtmScatteringBytes]
//   [single-Mie scattering : AtmScatteringBytes]   // omitted iff combined_scattering
//   [irradiance   LUT      : AtmIrradianceBytes]
//
// When `combined_scattering` is non-zero, the single-Mie LUT block is
// skipped (its data is folded into the scattering LUT alpha channel,
// per Bruneton's COMBINED_SCATTERING_TEXTURES path).
struct AtmFileHeader
{
    std::uint32_t magic;                     // = AtmFileMagic
    std::uint32_t version;                   // = AtmFileVersion
    std::uint32_t combined_scattering;       // 0 = separate Mie LUT, 1 = combined
    std::uint32_t _reserved0;                // pad to 16 bytes

    // LUT dimensions, repeated in the header for sanity-checking against
    // the shader's compile-time const ints.
    std::uint32_t transmittance_width;
    std::uint32_t transmittance_height;
    std::uint32_t scattering_r_size;
    std::uint32_t scattering_mu_size;
    std::uint32_t scattering_mu_s_size;
    std::uint32_t scattering_nu_size;
    std::uint32_t irradiance_width;
    std::uint32_t irradiance_height;

    // Per-body atmosphere parameters bake-time. Same std140 layout as
    // the AtmosphereBlock UBO so it can be memcpy'd straight in (minus
    // the two trailing radiance-to-luminance vec3s, which are derived
    // from the spectral integration the bake tool performs and follow
    // the parameters here).
    Std140AtmosphereParameters parameters;

    float sky_spectral_radiance_to_luminance[3];
    float _pad_sky;
    float sun_spectral_radiance_to_luminance[3];
    float _pad_sun;
};

static_assert(sizeof(AtmFileHeader) ==
              16 /*magic..reserved0*/ +
              32 /*dimensions*/ +
              sizeof(Std140AtmosphereParameters) +
              32 /*two padded vec3s*/,
              "AtmFileHeader layout drift");

} // namespace celestia::engine
