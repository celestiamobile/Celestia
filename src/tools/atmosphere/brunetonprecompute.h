// Copyright (c) 2017 Eric Bruneton
// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: BSD-3-Clause

// A standalone, CPU-only C++17 port of Eric Bruneton's
// precomputed atmospheric scattering pipeline (RGB / 3-wavelength path, as used
// by the demo with use_luminance == NONE and combined scattering textures).
//
// This mirrors atmosphere/functions.glsl and the multi-order driver in
// atmosphere/reference/model.cc (Algorithm 4.1). It produces the same four
// LUTs the GPU pipeline bakes into a Celestia .atm file.

#pragma once

#include <vector>

#include "brunetonvec.h"

namespace bruneton {

// LUT dimensions (from atmosphere/constants.h).
constexpr int TRANSMITTANCE_TEXTURE_WIDTH = 256;
constexpr int TRANSMITTANCE_TEXTURE_HEIGHT = 64;
constexpr int SCATTERING_TEXTURE_R_SIZE = 32;
constexpr int SCATTERING_TEXTURE_MU_SIZE = 128;
constexpr int SCATTERING_TEXTURE_MU_S_SIZE = 32;
constexpr int SCATTERING_TEXTURE_NU_SIZE = 8;
constexpr int SCATTERING_TEXTURE_WIDTH =
    SCATTERING_TEXTURE_NU_SIZE * SCATTERING_TEXTURE_MU_S_SIZE;   // 256
constexpr int SCATTERING_TEXTURE_HEIGHT = SCATTERING_TEXTURE_MU_SIZE;  // 128
constexpr int SCATTERING_TEXTURE_DEPTH = SCATTERING_TEXTURE_R_SIZE;    // 32
constexpr int IRRADIANCE_TEXTURE_WIDTH = 64;
constexpr int IRRADIANCE_TEXTURE_HEIGHT = 16;

// Matches Bruneton's DensityProfileLayer / DensityProfile.
struct DensityProfileLayer {
  double width = 0.0;
  double exp_term = 0.0;
  double exp_scale = 0.0;
  double linear_term = 0.0;
  double constant_term = 0.0;
  DensityProfileLayer() = default;
  DensityProfileLayer(double w, double et, double es, double lt, double ct)
      : width(w), exp_term(et), exp_scale(es), linear_term(lt),
        constant_term(ct) {}
};

struct DensityProfile {
  DensityProfileLayer layers[2];
};

// All parameters are expressed in the shader length unit (km for the demo:
// length_unit_in_meters == 1000). Scattering/extinction coefficients are the
// per-wavelength values already multiplied by length_unit_in_meters, exactly as
// bake_atm.cc does. solar_irradiance, ground_albedo are unit-scaled.
struct AtmosphereParameters {
  dvec3 solar_irradiance;
  double sun_angular_radius = 0.0;
  double bottom_radius = 0.0;
  double top_radius = 0.0;
  DensityProfile rayleigh_density;
  dvec3 rayleigh_scattering;
  DensityProfile mie_density;
  dvec3 mie_scattering;
  dvec3 mie_extinction;
  double mie_phase_function_g = 0.0;
  DensityProfile absorption_density;
  dvec3 absorption_extinction;
  dvec3 ground_albedo;
  double mu_s_min = 0.0;
};

// A 2D texture of dvec3 with GL_LINEAR / GL_CLAMP_TO_EDGE sampling semantics.
struct Tex2 {
  int w = 0, h = 0;
  std::vector<dvec3> data;
  void Alloc(int w_, int h_) { w = w_; h = h_; data.assign(size_t(w) * h, dvec3()); }
  dvec3& At(int i, int j) { return data[size_t(j) * w + i]; }
  const dvec3& At(int i, int j) const { return data[size_t(j) * w + i]; }
  dvec3 Sample(double u, double v) const;
};

// A 3D texture of dvec3 with GL_LINEAR / GL_CLAMP_TO_EDGE sampling semantics.
struct Tex3 {
  int w = 0, h = 0, d = 0;
  std::vector<dvec3> data;
  void Alloc(int w_, int h_, int d_) {
    w = w_; h = h_; d = d_;
    data.assign(size_t(w) * h * d, dvec3());
  }
  dvec3& At(int i, int j, int k) {
    return data[(size_t(k) * h + j) * w + i];
  }
  const dvec3& At(int i, int j, int k) const {
    return data[(size_t(k) * h + j) * w + i];
  }
  dvec3 Sample(double u, double v, double w_coord) const;
};

// Result holder: the four LUTs, plus RGBA32F output buffers.
struct PrecomputedTextures {
  Tex2 transmittance;
  Tex3 scattering;       // Rayleigh + multiple scattering (divided by rayleigh phase)
  Tex3 single_mie;       // single Mie scattering (RGB)
  Tex2 irradiance;

  // Produce RGBA32F little-endian float arrays matching bake_atm.cc's layout.
  // combined == true packs single_mie.r into scattering.a and does not emit a
  // separate single_mie array (this matches the baked earth.atm file).
  std::vector<float> TransmittanceRGBA() const;    // W*H*4
  std::vector<float> ScatteringRGBA(bool combined) const;  // W*H*D*4
  std::vector<float> SingleMieRGBA() const;         // W*H*D*4 (combined==false)
  std::vector<float> IrradianceRGBA() const;        // W*H*4
};

struct PrecomputeSettings {
  int scattering_orders = 4;
  int thread_count = 0;  // 0 selects the hardware concurrency.
  // Round every 3D texture store to binary16, matching the reference GPU path.
  bool emulate_half_precision = true;
};

// Runs Algorithm 4.1 (transmittance -> direct irradiance -> single scattering
// -> [scattering density -> indirect irradiance -> multiple scattering] for
// orders 2..num_scattering_orders). Multithreaded across texture rows/slices.
PrecomputedTextures Precompute(const AtmosphereParameters& atmosphere,
                               const PrecomputeSettings& settings = {});

}  // namespace bruneton
