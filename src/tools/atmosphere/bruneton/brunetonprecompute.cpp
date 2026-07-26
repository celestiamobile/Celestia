// Derived from Eric Bruneton's BSD-3-Clause implementation; see LICENSE.
// brunetonprecompute.cpp — see header. Direct CPU port of the precompute-
// relevant functions in atmosphere/functions.glsl, driven in the order defined
// by atmosphere/reference/model.cc (Algorithm 4.1).
//
// All GLSL unit symbols (m, m2, rad, sr, nm, watt, watt_per_*) equal 1.0, so we
// drop them. Everything is computed in double precision.

#include "brunetonprecompute.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace bruneton {

bool g_emulate_half = false;

namespace {

constexpr double PI = 3.14159265358979323846;

// ---- texture sampling (GL_LINEAR + GL_CLAMP_TO_EDGE, texel-center) ---------

inline dvec3 lerp3(const dvec3& a, const dvec3& b, double t) {
  return a * (1.0 - t) + b * t;
}

}  // namespace

dvec3 Tex2::Sample(double u, double v) const {
  double tx = u * w - 0.5;
  double ty = v * h - 0.5;
  double fx = std::floor(tx), fy = std::floor(ty);
  double ax = tx - fx, ay = ty - fy;
  int x0 = int(fx), y0 = int(fy);
  int x1 = x0 + 1, y1 = y0 + 1;
  x0 = x0 < 0 ? 0 : (x0 > w - 1 ? w - 1 : x0);
  x1 = x1 < 0 ? 0 : (x1 > w - 1 ? w - 1 : x1);
  y0 = y0 < 0 ? 0 : (y0 > h - 1 ? h - 1 : y0);
  y1 = y1 < 0 ? 0 : (y1 > h - 1 ? h - 1 : y1);
  dvec3 c0 = lerp3(At(x0, y0), At(x1, y0), ax);
  dvec3 c1 = lerp3(At(x0, y1), At(x1, y1), ax);
  return lerp3(c0, c1, ay);
}

dvec3 Tex3::Sample(double u, double v, double wc) const {
  double tx = u * w - 0.5;
  double ty = v * h - 0.5;
  double tz = wc * d - 0.5;
  double fx = std::floor(tx), fy = std::floor(ty), fz = std::floor(tz);
  double ax = tx - fx, ay = ty - fy, az = tz - fz;
  int x0 = int(fx), y0 = int(fy), z0 = int(fz);
  int x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
  x0 = x0 < 0 ? 0 : (x0 > w - 1 ? w - 1 : x0);
  x1 = x1 < 0 ? 0 : (x1 > w - 1 ? w - 1 : x1);
  y0 = y0 < 0 ? 0 : (y0 > h - 1 ? h - 1 : y0);
  y1 = y1 < 0 ? 0 : (y1 > h - 1 ? h - 1 : y1);
  z0 = z0 < 0 ? 0 : (z0 > d - 1 ? d - 1 : z0);
  z1 = z1 < 0 ? 0 : (z1 > d - 1 ? d - 1 : z1);
  dvec3 c00 = lerp3(At(x0, y0, z0), At(x1, y0, z0), ax);
  dvec3 c10 = lerp3(At(x0, y1, z0), At(x1, y1, z0), ax);
  dvec3 c01 = lerp3(At(x0, y0, z1), At(x1, y0, z1), ax);
  dvec3 c11 = lerp3(At(x0, y1, z1), At(x1, y1, z1), ax);
  dvec3 c0 = lerp3(c00, c10, ay);
  dvec3 c1 = lerp3(c01, c11, ay);
  return lerp3(c0, c1, az);
}

// ===========================================================================
// Ported functions.glsl. Argument order and bodies follow the shader exactly.
// ===========================================================================

namespace {

using Atm = AtmosphereParameters;

double ClampCosine(double mu) { return clampd(mu, -1.0, 1.0); }
double ClampDistance(double d) { return std::max(d, 0.0); }
double ClampRadius(const Atm& atm, double r) {
  return clampd(r, atm.bottom_radius, atm.top_radius);
}
double SafeSqrt(double a) { return std::sqrt(std::max(a, 0.0)); }

double DistanceToTopAtmosphereBoundary(const Atm& atm, double r, double mu) {
  double discriminant = r * r * (mu * mu - 1.0) + atm.top_radius * atm.top_radius;
  return ClampDistance(-r * mu + SafeSqrt(discriminant));
}

double DistanceToBottomAtmosphereBoundary(const Atm& atm, double r, double mu) {
  double discriminant =
      r * r * (mu * mu - 1.0) + atm.bottom_radius * atm.bottom_radius;
  return ClampDistance(-r * mu - SafeSqrt(discriminant));
}

bool RayIntersectsGround(const Atm& atm, double r, double mu) {
  return mu < 0.0 &&
      r * r * (mu * mu - 1.0) + atm.bottom_radius * atm.bottom_radius >= 0.0;
}

double GetLayerDensity(const DensityProfileLayer& layer, double altitude) {
  double density = layer.exp_term * std::exp(layer.exp_scale * altitude) +
      layer.linear_term * altitude + layer.constant_term;
  return clampd(density, 0.0, 1.0);
}

double GetProfileDensity(const DensityProfile& profile, double altitude) {
  return altitude < profile.layers[0].width
             ? GetLayerDensity(profile.layers[0], altitude)
             : GetLayerDensity(profile.layers[1], altitude);
}

double ComputeOpticalLengthToTopAtmosphereBoundary(
    const Atm& atm, const DensityProfile& profile, double r, double mu) {
  const int SAMPLE_COUNT = 500;
  double dx = DistanceToTopAtmosphereBoundary(atm, r, mu) / double(SAMPLE_COUNT);
  double result = 0.0;
  for (int i = 0; i <= SAMPLE_COUNT; ++i) {
    double d_i = double(i) * dx;
    double r_i = std::sqrt(d_i * d_i + 2.0 * r * mu * d_i + r * r);
    double y_i = GetProfileDensity(profile, r_i - atm.bottom_radius);
    double weight_i = (i == 0 || i == SAMPLE_COUNT) ? 0.5 : 1.0;
    result += y_i * weight_i * dx;
  }
  return result;
}

dvec3 ComputeTransmittanceToTopAtmosphereBoundary(const Atm& atm, double r,
                                                  double mu) {
  return vexp(-(
      atm.rayleigh_scattering *
          ComputeOpticalLengthToTopAtmosphereBoundary(
              atm, atm.rayleigh_density, r, mu) +
      atm.mie_extinction *
          ComputeOpticalLengthToTopAtmosphereBoundary(
              atm, atm.mie_density, r, mu) +
      atm.absorption_extinction *
          ComputeOpticalLengthToTopAtmosphereBoundary(
              atm, atm.absorption_density, r, mu)));
}

double GetTextureCoordFromUnitRange(double x, int texture_size) {
  return 0.5 / double(texture_size) + x * (1.0 - 1.0 / double(texture_size));
}

double GetUnitRangeFromTextureCoord(double u, int texture_size) {
  return (u - 0.5 / double(texture_size)) / (1.0 - 1.0 / double(texture_size));
}

dvec2 GetTransmittanceTextureUvFromRMu(const Atm& atm, double r, double mu) {
  double H = std::sqrt(atm.top_radius * atm.top_radius -
                       atm.bottom_radius * atm.bottom_radius);
  double rho = SafeSqrt(r * r - atm.bottom_radius * atm.bottom_radius);
  double d = DistanceToTopAtmosphereBoundary(atm, r, mu);
  double d_min = atm.top_radius - r;
  double d_max = rho + H;
  double x_mu = (d - d_min) / (d_max - d_min);
  double x_r = rho / H;
  return dvec2(GetTextureCoordFromUnitRange(x_mu, TRANSMITTANCE_TEXTURE_WIDTH),
               GetTextureCoordFromUnitRange(x_r, TRANSMITTANCE_TEXTURE_HEIGHT));
}

void GetRMuFromTransmittanceTextureUv(const Atm& atm, const dvec2& uv,
                                      double& r, double& mu) {
  double x_mu = GetUnitRangeFromTextureCoord(uv.x, TRANSMITTANCE_TEXTURE_WIDTH);
  double x_r = GetUnitRangeFromTextureCoord(uv.y, TRANSMITTANCE_TEXTURE_HEIGHT);
  double H = std::sqrt(atm.top_radius * atm.top_radius -
                       atm.bottom_radius * atm.bottom_radius);
  double rho = H * x_r;
  r = std::sqrt(rho * rho + atm.bottom_radius * atm.bottom_radius);
  double d_min = atm.top_radius - r;
  double d_max = rho + H;
  double d = d_min + x_mu * (d_max - d_min);
  mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
  mu = ClampCosine(mu);
}

dvec3 ComputeTransmittanceToTopAtmosphereBoundaryTexture(const Atm& atm,
                                                         const dvec2& frag) {
  double r, mu;
  GetRMuFromTransmittanceTextureUv(
      atm,
      dvec2(frag.x / TRANSMITTANCE_TEXTURE_WIDTH,
            frag.y / TRANSMITTANCE_TEXTURE_HEIGHT),
      r, mu);
  return ComputeTransmittanceToTopAtmosphereBoundary(atm, r, mu);
}

dvec3 GetTransmittanceToTopAtmosphereBoundary(const Atm& atm, const Tex2& tex,
                                              double r, double mu) {
  dvec2 uv = GetTransmittanceTextureUvFromRMu(atm, r, mu);
  return tex.Sample(uv.x, uv.y);
}

dvec3 GetTransmittance(const Atm& atm, const Tex2& tex, double r, double mu,
                       double d, bool ray_r_mu_intersects_ground) {
  double r_d = ClampRadius(atm, std::sqrt(d * d + 2.0 * r * mu * d + r * r));
  double mu_d = ClampCosine((r * mu + d) / r_d);
  if (ray_r_mu_intersects_ground) {
    return vmin(GetTransmittanceToTopAtmosphereBoundary(atm, tex, r_d, -mu_d) /
                    GetTransmittanceToTopAtmosphereBoundary(atm, tex, r, -mu),
                1.0);
  } else {
    return vmin(GetTransmittanceToTopAtmosphereBoundary(atm, tex, r, mu) /
                    GetTransmittanceToTopAtmosphereBoundary(atm, tex, r_d, mu_d),
                1.0);
  }
}

dvec3 GetTransmittanceToSun(const Atm& atm, const Tex2& tex, double r,
                            double mu_s) {
  double sin_theta_h = atm.bottom_radius / r;
  double cos_theta_h = -std::sqrt(std::max(1.0 - sin_theta_h * sin_theta_h, 0.0));
  return GetTransmittanceToTopAtmosphereBoundary(atm, tex, r, mu_s) *
         smoothstepd(-sin_theta_h * atm.sun_angular_radius,
                     sin_theta_h * atm.sun_angular_radius, mu_s - cos_theta_h);
}

// ---- single scattering -----------------------------------------------------

void ComputeSingleScatteringIntegrand(const Atm& atm, const Tex2& tex, double r,
                                      double mu, double mu_s, double nu,
                                      double d, bool ray_r_mu_intersects_ground,
                                      dvec3& rayleigh, dvec3& mie) {
  double r_d = ClampRadius(atm, std::sqrt(d * d + 2.0 * r * mu * d + r * r));
  double mu_s_d = ClampCosine((r * mu_s + d * nu) / r_d);
  dvec3 transmittance =
      GetTransmittance(atm, tex, r, mu, d, ray_r_mu_intersects_ground) *
      GetTransmittanceToSun(atm, tex, r_d, mu_s_d);
  rayleigh = transmittance *
             GetProfileDensity(atm.rayleigh_density, r_d - atm.bottom_radius);
  mie = transmittance *
        GetProfileDensity(atm.mie_density, r_d - atm.bottom_radius);
}

double DistanceToNearestAtmosphereBoundary(const Atm& atm, double r, double mu,
                                           bool ray_r_mu_intersects_ground) {
  return ray_r_mu_intersects_ground
             ? DistanceToBottomAtmosphereBoundary(atm, r, mu)
             : DistanceToTopAtmosphereBoundary(atm, r, mu);
}

void ComputeSingleScattering(const Atm& atm, const Tex2& tex, double r,
                             double mu, double mu_s, double nu,
                             bool ray_r_mu_intersects_ground, dvec3& rayleigh,
                             dvec3& mie) {
  const int SAMPLE_COUNT = 50;
  double dx = DistanceToNearestAtmosphereBoundary(
                  atm, r, mu, ray_r_mu_intersects_ground) /
              double(SAMPLE_COUNT);
  dvec3 rayleigh_sum(0.0), mie_sum(0.0);
  for (int i = 0; i <= SAMPLE_COUNT; ++i) {
    double d_i = double(i) * dx;
    dvec3 rayleigh_i, mie_i;
    ComputeSingleScatteringIntegrand(atm, tex, r, mu, mu_s, nu, d_i,
                                     ray_r_mu_intersects_ground, rayleigh_i,
                                     mie_i);
    double weight_i = (i == 0 || i == SAMPLE_COUNT) ? 0.5 : 1.0;
    rayleigh_sum += rayleigh_i * weight_i;
    mie_sum += mie_i * weight_i;
  }
  rayleigh = rayleigh_sum * dx * atm.solar_irradiance * atm.rayleigh_scattering;
  mie = mie_sum * dx * atm.solar_irradiance * atm.mie_scattering;
}

double RayleighPhaseFunction(double nu) {
  double k = 3.0 / (16.0 * PI);
  return k * (1.0 + nu * nu);
}

double MiePhaseFunction(double g, double nu) {
  double k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
  return k * (1.0 + nu * nu) / std::pow(1.0 + g * g - 2.0 * g * nu, 1.5);
}

dvec4 GetScatteringTextureUvwzFromRMuMuSNu(const Atm& atm, double r, double mu,
                                           double mu_s, double nu,
                                           bool ray_r_mu_intersects_ground) {
  double H = std::sqrt(atm.top_radius * atm.top_radius -
                       atm.bottom_radius * atm.bottom_radius);
  double rho = SafeSqrt(r * r - atm.bottom_radius * atm.bottom_radius);
  double u_r = GetTextureCoordFromUnitRange(rho / H, SCATTERING_TEXTURE_R_SIZE);

  double r_mu = r * mu;
  double discriminant =
      r_mu * r_mu - r * r + atm.bottom_radius * atm.bottom_radius;
  double u_mu;
  if (ray_r_mu_intersects_ground) {
    double d = -r_mu - SafeSqrt(discriminant);
    double d_min = r - atm.bottom_radius;
    double d_max = rho;
    u_mu = 0.5 - 0.5 * GetTextureCoordFromUnitRange(
                           d_max == d_min ? 0.0 : (d - d_min) / (d_max - d_min),
                           SCATTERING_TEXTURE_MU_SIZE / 2);
  } else {
    double d = -r_mu + SafeSqrt(discriminant + H * H);
    double d_min = atm.top_radius - r;
    double d_max = rho + H;
    u_mu = 0.5 + 0.5 * GetTextureCoordFromUnitRange(
                           (d - d_min) / (d_max - d_min),
                           SCATTERING_TEXTURE_MU_SIZE / 2);
  }

  double d = DistanceToTopAtmosphereBoundary(atm, atm.bottom_radius, mu_s);
  double d_min = atm.top_radius - atm.bottom_radius;
  double d_max = H;
  double a = (d - d_min) / (d_max - d_min);
  double D = DistanceToTopAtmosphereBoundary(atm, atm.bottom_radius, atm.mu_s_min);
  double A = (D - d_min) / (d_max - d_min);
  double u_mu_s = GetTextureCoordFromUnitRange(
      std::max(1.0 - a / A, 0.0) / (1.0 + a), SCATTERING_TEXTURE_MU_S_SIZE);

  double u_nu = (nu + 1.0) / 2.0;
  return dvec4(u_nu, u_mu_s, u_mu, u_r);
}

void GetRMuMuSNuFromScatteringTextureUvwz(const Atm& atm, const dvec4& uvwz,
                                          double& r, double& mu, double& mu_s,
                                          double& nu,
                                          bool& ray_r_mu_intersects_ground) {
  double H = std::sqrt(atm.top_radius * atm.top_radius -
                       atm.bottom_radius * atm.bottom_radius);
  double rho = H * GetUnitRangeFromTextureCoord(uvwz.w, SCATTERING_TEXTURE_R_SIZE);
  r = std::sqrt(rho * rho + atm.bottom_radius * atm.bottom_radius);

  if (uvwz.z < 0.5) {
    double d_min = r - atm.bottom_radius;
    double d_max = rho;
    double d = d_min + (d_max - d_min) * GetUnitRangeFromTextureCoord(
                                             1.0 - 2.0 * uvwz.z,
                                             SCATTERING_TEXTURE_MU_SIZE / 2);
    mu = d == 0.0 ? -1.0 : ClampCosine(-(rho * rho + d * d) / (2.0 * r * d));
    ray_r_mu_intersects_ground = true;
  } else {
    double d_min = atm.top_radius - r;
    double d_max = rho + H;
    double d = d_min + (d_max - d_min) * GetUnitRangeFromTextureCoord(
                                             2.0 * uvwz.z - 1.0,
                                             SCATTERING_TEXTURE_MU_SIZE / 2);
    mu = d == 0.0 ? 1.0
                  : ClampCosine((H * H - rho * rho - d * d) / (2.0 * r * d));
    ray_r_mu_intersects_ground = false;
  }

  double x_mu_s = GetUnitRangeFromTextureCoord(uvwz.y, SCATTERING_TEXTURE_MU_S_SIZE);
  double d_min = atm.top_radius - atm.bottom_radius;
  double d_max = H;
  double D = DistanceToTopAtmosphereBoundary(atm, atm.bottom_radius, atm.mu_s_min);
  double A = (D - d_min) / (d_max - d_min);
  double a = (A - x_mu_s * A) / (1.0 + x_mu_s * A);
  double d = d_min + std::min(a, A) * (d_max - d_min);
  mu_s = d == 0.0 ? 1.0
                  : ClampCosine((H * H - d * d) / (2.0 * atm.bottom_radius * d));

  nu = ClampCosine(uvwz.x * 2.0 - 1.0);
}

void GetRMuMuSNuFromScatteringTextureFragCoord(const Atm& atm,
                                               const dvec3& frag, double& r,
                                               double& mu, double& mu_s,
                                               double& nu,
                                               bool& ray_r_mu_intersects_ground) {
  const dvec4 SIZE = dvec4(SCATTERING_TEXTURE_NU_SIZE - 1,
                           SCATTERING_TEXTURE_MU_S_SIZE,
                           SCATTERING_TEXTURE_MU_SIZE, SCATTERING_TEXTURE_R_SIZE);
  double frag_coord_nu = std::floor(frag.x / double(SCATTERING_TEXTURE_MU_S_SIZE));
  double frag_coord_mu_s = std::fmod(frag.x, double(SCATTERING_TEXTURE_MU_S_SIZE));
  dvec4 uvwz = dvec4(frag_coord_nu / SIZE.x, frag_coord_mu_s / SIZE.y,
                     frag.y / SIZE.z, frag.z / SIZE.w);
  GetRMuMuSNuFromScatteringTextureUvwz(atm, uvwz, r, mu, mu_s, nu,
                                       ray_r_mu_intersects_ground);
  nu = clampd(nu, mu * mu_s - std::sqrt((1.0 - mu * mu) * (1.0 - mu_s * mu_s)),
              mu * mu_s + std::sqrt((1.0 - mu * mu) * (1.0 - mu_s * mu_s)));
}

void ComputeSingleScatteringTexture(const Atm& atm, const Tex2& tex,
                                    const dvec3& frag, dvec3& rayleigh,
                                    dvec3& mie) {
  double r, mu, mu_s, nu;
  bool ground;
  GetRMuMuSNuFromScatteringTextureFragCoord(atm, frag, r, mu, mu_s, nu, ground);
  ComputeSingleScattering(atm, tex, r, mu, mu_s, nu, ground, rayleigh, mie);
}

// Single 4D lookup emulated with two 3D lookups (nu packing).
dvec3 GetScattering(const Atm& atm, const Tex3& scattering_texture, double r,
                    double mu, double mu_s, double nu,
                    bool ray_r_mu_intersects_ground) {
  dvec4 uvwz = GetScatteringTextureUvwzFromRMuMuSNu(atm, r, mu, mu_s, nu,
                                                    ray_r_mu_intersects_ground);
  double tex_coord_x = uvwz.x * double(SCATTERING_TEXTURE_NU_SIZE - 1);
  double tex_x = std::floor(tex_coord_x);
  double lerp = tex_coord_x - tex_x;
  double u0 = (tex_x + uvwz.y) / double(SCATTERING_TEXTURE_NU_SIZE);
  double u1 = (tex_x + 1.0 + uvwz.y) / double(SCATTERING_TEXTURE_NU_SIZE);
  return scattering_texture.Sample(u0, uvwz.z, uvwz.w) * (1.0 - lerp) +
         scattering_texture.Sample(u1, uvwz.z, uvwz.w) * lerp;
}

dvec3 GetScattering(const Atm& atm, const Tex3& single_rayleigh,
                    const Tex3& single_mie, const Tex3& multiple,
                    double r, double mu, double mu_s, double nu,
                    bool ray_r_mu_intersects_ground, int scattering_order) {
  if (scattering_order == 1) {
    dvec3 rayleigh = GetScattering(atm, single_rayleigh, r, mu, mu_s, nu,
                                   ray_r_mu_intersects_ground);
    dvec3 mie = GetScattering(atm, single_mie, r, mu, mu_s, nu,
                              ray_r_mu_intersects_ground);
    return rayleigh * RayleighPhaseFunction(nu) +
           mie * MiePhaseFunction(atm.mie_phase_function_g, nu);
  } else {
    return GetScattering(atm, multiple, r, mu, mu_s, nu,
                         ray_r_mu_intersects_ground);
  }
}

// ---- ground irradiance -----------------------------------------------------

dvec2 GetIrradianceTextureUvFromRMuS(const Atm& atm, double r, double mu_s) {
  double x_r = (r - atm.bottom_radius) / (atm.top_radius - atm.bottom_radius);
  double x_mu_s = mu_s * 0.5 + 0.5;
  return dvec2(GetTextureCoordFromUnitRange(x_mu_s, IRRADIANCE_TEXTURE_WIDTH),
               GetTextureCoordFromUnitRange(x_r, IRRADIANCE_TEXTURE_HEIGHT));
}

void GetRMuSFromIrradianceTextureUv(const Atm& atm, const dvec2& uv, double& r,
                                    double& mu_s) {
  double x_mu_s = GetUnitRangeFromTextureCoord(uv.x, IRRADIANCE_TEXTURE_WIDTH);
  double x_r = GetUnitRangeFromTextureCoord(uv.y, IRRADIANCE_TEXTURE_HEIGHT);
  r = atm.bottom_radius + x_r * (atm.top_radius - atm.bottom_radius);
  mu_s = ClampCosine(2.0 * x_mu_s - 1.0);
}

dvec3 GetIrradiance(const Atm& atm, const Tex2& tex, double r, double mu_s) {
  dvec2 uv = GetIrradianceTextureUvFromRMuS(atm, r, mu_s);
  return tex.Sample(uv.x, uv.y);
}

dvec3 ComputeDirectIrradiance(const Atm& atm, const Tex2& tex, double r,
                              double mu_s) {
  double alpha_s = atm.sun_angular_radius;
  double average_cosine_factor =
      mu_s < -alpha_s
          ? 0.0
          : (mu_s > alpha_s ? mu_s
                            : (mu_s + alpha_s) * (mu_s + alpha_s) /
                                  (4.0 * alpha_s));
  return atm.solar_irradiance *
         GetTransmittanceToTopAtmosphereBoundary(atm, tex, r, mu_s) *
         average_cosine_factor;
}

dvec3 ComputeIndirectIrradiance(const Atm& atm, const Tex3& single_rayleigh,
                                const Tex3& single_mie, const Tex3& multiple,
                                double r, double mu_s, int scattering_order) {
  const int SAMPLE_COUNT = 32;
  const double dphi = PI / double(SAMPLE_COUNT);
  const double dtheta = PI / double(SAMPLE_COUNT);
  dvec3 result(0.0);
  dvec3 omega_s(std::sqrt(1.0 - mu_s * mu_s), 0.0, mu_s);
  for (int j = 0; j < SAMPLE_COUNT / 2; ++j) {
    double theta = (double(j) + 0.5) * dtheta;
    for (int i = 0; i < 2 * SAMPLE_COUNT; ++i) {
      double phi = (double(i) + 0.5) * dphi;
      dvec3 omega(std::cos(phi) * std::sin(theta),
                  std::sin(phi) * std::sin(theta), std::cos(theta));
      double domega = dtheta * dphi * std::sin(theta);
      double nu = dot(omega, omega_s);
      result += GetScattering(atm, single_rayleigh, single_mie, multiple, r,
                              omega.z, mu_s, nu, false, scattering_order) *
                omega.z * domega;
    }
  }
  return result;
}

dvec3 ComputeDirectIrradianceTexture(const Atm& atm, const Tex2& tex,
                                     const dvec2& frag) {
  double r, mu_s;
  GetRMuSFromIrradianceTextureUv(
      atm,
      dvec2(frag.x / IRRADIANCE_TEXTURE_WIDTH, frag.y / IRRADIANCE_TEXTURE_HEIGHT),
      r, mu_s);
  return ComputeDirectIrradiance(atm, tex, r, mu_s);
}

dvec3 ComputeIndirectIrradianceTexture(const Atm& atm, const Tex3& single_rayleigh,
                                       const Tex3& single_mie,
                                       const Tex3& multiple, const dvec2& frag,
                                       int scattering_order) {
  double r, mu_s;
  GetRMuSFromIrradianceTextureUv(
      atm,
      dvec2(frag.x / IRRADIANCE_TEXTURE_WIDTH, frag.y / IRRADIANCE_TEXTURE_HEIGHT),
      r, mu_s);
  return ComputeIndirectIrradiance(atm, single_rayleigh, single_mie, multiple, r,
                                   mu_s, scattering_order);
}

// ---- multiple scattering ---------------------------------------------------

dvec3 ComputeScatteringDensity(const Atm& atm, const Tex2& transmittance,
                               const Tex3& single_rayleigh, const Tex3& single_mie,
                               const Tex3& multiple, const Tex2& irradiance,
                               double r, double mu, double mu_s, double nu,
                               int scattering_order) {
  dvec3 zenith_direction(0.0, 0.0, 1.0);
  dvec3 omega(std::sqrt(1.0 - mu * mu), 0.0, mu);
  double sun_dir_x = omega.x == 0.0 ? 0.0 : (nu - mu * mu_s) / omega.x;
  double sun_dir_y = std::sqrt(std::max(1.0 - sun_dir_x * sun_dir_x - mu_s * mu_s, 0.0));
  dvec3 omega_s(sun_dir_x, sun_dir_y, mu_s);

  const int SAMPLE_COUNT = 16;
  const double dphi = PI / double(SAMPLE_COUNT);
  const double dtheta = PI / double(SAMPLE_COUNT);
  dvec3 rayleigh_mie(0.0);

  for (int l = 0; l < SAMPLE_COUNT; ++l) {
    double theta = (double(l) + 0.5) * dtheta;
    double cos_theta = std::cos(theta);
    double sin_theta = std::sin(theta);
    bool ray_r_theta_intersects_ground = RayIntersectsGround(atm, r, cos_theta);

    double distance_to_ground = 0.0;
    dvec3 transmittance_to_ground(0.0);
    dvec3 ground_albedo(0.0);
    if (ray_r_theta_intersects_ground) {
      distance_to_ground = DistanceToBottomAtmosphereBoundary(atm, r, cos_theta);
      transmittance_to_ground = GetTransmittance(
          atm, transmittance, r, cos_theta, distance_to_ground, true);
      ground_albedo = atm.ground_albedo;
    }

    for (int m = 0; m < 2 * SAMPLE_COUNT; ++m) {
      double phi = (double(m) + 0.5) * dphi;
      dvec3 omega_i(std::cos(phi) * sin_theta, std::sin(phi) * sin_theta, cos_theta);
      double domega_i = dtheta * dphi * std::sin(theta);

      double nu1 = dot(omega_s, omega_i);
      dvec3 incident_radiance = GetScattering(
          atm, single_rayleigh, single_mie, multiple, r, omega_i.z, mu_s, nu1,
          ray_r_theta_intersects_ground, scattering_order - 1);

      dvec3 ground_normal =
          normalize(zenith_direction * r + omega_i * distance_to_ground);
      dvec3 ground_irradiance = GetIrradiance(
          atm, irradiance, atm.bottom_radius, dot(ground_normal, omega_s));
      incident_radiance += transmittance_to_ground * ground_albedo *
                           (1.0 / PI) * ground_irradiance;

      double nu2 = dot(omega, omega_i);
      double rayleigh_density =
          GetProfileDensity(atm.rayleigh_density, r - atm.bottom_radius);
      double mie_density =
          GetProfileDensity(atm.mie_density, r - atm.bottom_radius);
      rayleigh_mie += incident_radiance *
                      (atm.rayleigh_scattering * rayleigh_density *
                           RayleighPhaseFunction(nu2) +
                       atm.mie_scattering * mie_density *
                           MiePhaseFunction(atm.mie_phase_function_g, nu2)) *
                      domega_i;
    }
  }
  return rayleigh_mie;
}

dvec3 ComputeMultipleScattering(const Atm& atm, const Tex2& transmittance,
                                const Tex3& scattering_density, double r,
                                double mu, double mu_s, double nu,
                                bool ray_r_mu_intersects_ground) {
  const int SAMPLE_COUNT = 50;
  double dx = DistanceToNearestAtmosphereBoundary(
                  atm, r, mu, ray_r_mu_intersects_ground) /
              double(SAMPLE_COUNT);
  dvec3 rayleigh_mie_sum(0.0);
  for (int i = 0; i <= SAMPLE_COUNT; ++i) {
    double d_i = double(i) * dx;
    double r_i = ClampRadius(atm, std::sqrt(d_i * d_i + 2.0 * r * mu * d_i + r * r));
    double mu_i = ClampCosine((r * mu + d_i) / r_i);
    double mu_s_i = ClampCosine((r * mu_s + d_i * nu) / r_i);
    dvec3 rayleigh_mie_i =
        GetScattering(atm, scattering_density, r_i, mu_i, mu_s_i, nu,
                      ray_r_mu_intersects_ground) *
        GetTransmittance(atm, transmittance, r, mu, d_i,
                         ray_r_mu_intersects_ground) *
        dx;
    double weight_i = (i == 0 || i == SAMPLE_COUNT) ? 0.5 : 1.0;
    rayleigh_mie_sum += rayleigh_mie_i * weight_i;
  }
  return rayleigh_mie_sum;
}

dvec3 ComputeScatteringDensityTexture(const Atm& atm, const Tex2& transmittance,
                                      const Tex3& single_rayleigh,
                                      const Tex3& single_mie, const Tex3& multiple,
                                      const Tex2& irradiance, const dvec3& frag,
                                      int scattering_order) {
  double r, mu, mu_s, nu;
  bool ground;
  GetRMuMuSNuFromScatteringTextureFragCoord(atm, frag, r, mu, mu_s, nu, ground);
  return ComputeScatteringDensity(atm, transmittance, single_rayleigh, single_mie,
                                  multiple, irradiance, r, mu, mu_s, nu,
                                  scattering_order);
}

dvec3 ComputeMultipleScatteringTexture(const Atm& atm, const Tex2& transmittance,
                                       const Tex3& scattering_density,
                                       const dvec3& frag, double& nu) {
  double r, mu, mu_s;
  bool ground;
  GetRMuMuSNuFromScatteringTextureFragCoord(atm, frag, r, mu, mu_s, nu, ground);
  return ComputeMultipleScattering(atm, transmittance, scattering_density, r, mu,
                                   mu_s, nu, ground);
}

// ---- tiny thread pool over a 1D job range ---------------------------------

// Round a value to IEEE half precision (round-to-nearest-even) and back to
// double, matching the RGBA16F storage the GPU uses for all 3D scattering
// textures when use_half_precision is enabled. Handles the value ranges that
// occur here (finite, |x| < 65504).
double ToHalf(double xd) {
  float xf = float(xd);
  uint32_t f;
  std::memcpy(&f, &xf, 4);
  uint32_t sign = (f >> 16) & 0x8000u;
  int32_t exp = int32_t((f >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = f & 0x7FFFFFu;
  uint16_t h;
  if (((f >> 23) & 0xFF) == 0xFF) {
    h = uint16_t(sign | 0x7C00u | (mant ? 0x200u : 0));  // inf/nan
  } else if (exp >= 0x1F) {
    h = uint16_t(sign | 0x7C00u);  // overflow -> inf
  } else if (exp <= 0) {
    if (exp < -10) {
      h = uint16_t(sign);  // underflow -> 0
    } else {
      mant |= 0x800000u;
      int shift = 14 - exp;
      uint32_t m = mant >> shift;
      uint32_t rem = mant & ((1u << shift) - 1);
      uint32_t half = 1u << (shift - 1);
      if (rem > half || (rem == half && (m & 1))) m++;
      h = uint16_t(sign | m);
    }
  } else {
    uint32_t m = mant >> 13;
    uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1))) {
      m++;
      if (m == 0x400u) { m = 0; exp++; if (exp >= 0x1F) return double(std::copysign(INFINITY, xd)); }
    }
    h = uint16_t(sign | (uint32_t(exp) << 10) | m);
  }
  // half -> float
  uint32_t hs = (h & 0x8000u) << 16;
  uint32_t he = (h >> 10) & 0x1F;
  uint32_t hm = h & 0x3FFu;
  uint32_t out;
  if (he == 0) {
    if (hm == 0) { out = hs; }
    else {
      int e = -1;
      do { e++; hm <<= 1; } while ((hm & 0x400u) == 0);
      hm &= 0x3FFu;
      out = hs | (uint32_t(127 - 15 - e) << 23) | (hm << 13);
    }
  } else if (he == 0x1F) {
    out = hs | 0x7F800000u | (hm << 13);
  } else {
    out = hs | (uint32_t(he - 15 + 127) << 23) | (hm << 13);
  }
  float of;
  std::memcpy(&of, &out, 4);
  return double(of);
}

inline dvec3 ToHalf3(const dvec3& v) {
  return dvec3(ToHalf(v.x), ToHalf(v.y), ToHalf(v.z));
}

// ---- tiny thread pool over a 1D job range ---------------------------------

template <class Fn>
void RunJobs(Fn fn, int count, int num_threads) {
  if (num_threads <= 1) {
    for (int i = 0; i < count; ++i) fn(i);
    return;
  }
  std::vector<std::thread> threads;
  std::atomic<int> next{0};
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&]() {
      for (;;) {
        int i = next.fetch_add(1);
        if (i >= count) break;
        fn(i);
      }
    });
  }
  for (auto& th : threads) th.join();
}

}  // namespace

// ===========================================================================
// Driver: Algorithm 4.1 (mirrors atmosphere/reference/model.cc Init()).
// ===========================================================================

PrecomputedTextures Precompute(const AtmosphereParameters& atm,
                               int num_scattering_orders, int num_threads) {
  const bool emulate_half = g_emulate_half;
  auto H3 = [&](const dvec3& v) { return emulate_half ? ToHalf3(v) : v; };
  if (num_threads <= 0) {
    num_threads = int(std::thread::hardware_concurrency());
    if (num_threads <= 0) num_threads = 1;
  }

  PrecomputedTextures out;
  out.transmittance.Alloc(TRANSMITTANCE_TEXTURE_WIDTH, TRANSMITTANCE_TEXTURE_HEIGHT);
  out.scattering.Alloc(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
                       SCATTERING_TEXTURE_DEPTH);
  out.single_mie.Alloc(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
                       SCATTERING_TEXTURE_DEPTH);
  out.irradiance.Alloc(IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT);

  // Temporary textures for the multi-order algorithm.
  Tex2 delta_irradiance;
  delta_irradiance.Alloc(IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT);
  Tex3 delta_rayleigh;
  delta_rayleigh.Alloc(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
                       SCATTERING_TEXTURE_DEPTH);
  Tex3& delta_mie = out.single_mie;  // reuse output as the delta-mie scratch
  Tex3 delta_scattering_density;
  delta_scattering_density.Alloc(SCATTERING_TEXTURE_WIDTH,
                                 SCATTERING_TEXTURE_HEIGHT,
                                 SCATTERING_TEXTURE_DEPTH);
  Tex3 delta_multiple_scattering;
  delta_multiple_scattering.Alloc(SCATTERING_TEXTURE_WIDTH,
                                  SCATTERING_TEXTURE_HEIGHT,
                                  SCATTERING_TEXTURE_DEPTH);

  // 1. Transmittance.
  RunJobs([&](int j) {
    for (int i = 0; i < TRANSMITTANCE_TEXTURE_WIDTH; ++i) {
      out.transmittance.At(i, j) =
          ComputeTransmittanceToTopAtmosphereBoundaryTexture(
              atm, dvec2(i + 0.5, j + 0.5));
    }
  }, TRANSMITTANCE_TEXTURE_HEIGHT, num_threads);

  // 2. Direct irradiance -> delta_irradiance; irradiance_texture := 0.
  RunJobs([&](int j) {
    for (int i = 0; i < IRRADIANCE_TEXTURE_WIDTH; ++i) {
      delta_irradiance.At(i, j) = ComputeDirectIrradianceTexture(
          atm, out.transmittance, dvec2(i + 0.5, j + 0.5));
      out.irradiance.At(i, j) = dvec3(0.0);
    }
  }, IRRADIANCE_TEXTURE_HEIGHT, num_threads);

  // 3. Single scattering -> delta_rayleigh, delta_mie; scattering := rayleigh.
  RunJobs([&](int k) {
    for (int j = 0; j < SCATTERING_TEXTURE_HEIGHT; ++j) {
      for (int i = 0; i < SCATTERING_TEXTURE_WIDTH; ++i) {
        dvec3 rayleigh, mie;
        ComputeSingleScatteringTexture(atm, out.transmittance,
                                       dvec3(i + 0.5, j + 0.5, k + 0.5),
                                       rayleigh, mie);
        delta_rayleigh.At(i, j, k) = H3(rayleigh);
        delta_mie.At(i, j, k) = H3(mie);
        out.scattering.At(i, j, k) = H3(rayleigh);
      }
    }
  }, SCATTERING_TEXTURE_DEPTH, num_threads);

  // 4. Orders 2..N.
  for (int scattering_order = 2; scattering_order <= num_scattering_orders;
       ++scattering_order) {
    // 4a. Scattering density.
    RunJobs([&](int k) {
      for (int j = 0; j < SCATTERING_TEXTURE_HEIGHT; ++j) {
        for (int i = 0; i < SCATTERING_TEXTURE_WIDTH; ++i) {
          delta_scattering_density.At(i, j, k) = H3(ComputeScatteringDensityTexture(
              atm, out.transmittance, delta_rayleigh, delta_mie,
              delta_multiple_scattering, delta_irradiance,
              dvec3(i + 0.5, j + 0.5, k + 0.5), scattering_order));
        }
      }
    }, SCATTERING_TEXTURE_DEPTH, num_threads);

    // 4b. Indirect irradiance -> delta_irradiance; accumulate into irradiance.
    RunJobs([&](int j) {
      for (int i = 0; i < IRRADIANCE_TEXTURE_WIDTH; ++i) {
        delta_irradiance.At(i, j) = ComputeIndirectIrradianceTexture(
            atm, delta_rayleigh, delta_mie, delta_multiple_scattering,
            dvec2(i + 0.5, j + 0.5), scattering_order - 1);
      }
    }, IRRADIANCE_TEXTURE_HEIGHT, num_threads);
    for (size_t n = 0; n < out.irradiance.data.size(); ++n)
      out.irradiance.data[n] += delta_irradiance.data[n];

    // 4c. Multiple scattering -> delta_multiple; accumulate into scattering.
    RunJobs([&](int k) {
      for (int j = 0; j < SCATTERING_TEXTURE_HEIGHT; ++j) {
        for (int i = 0; i < SCATTERING_TEXTURE_WIDTH; ++i) {
          double nu;
          dvec3 delta_ms = ComputeMultipleScatteringTexture(
              atm, out.transmittance, delta_scattering_density,
              dvec3(i + 0.5, j + 0.5, k + 0.5), nu);
          delta_multiple_scattering.At(i, j, k) = H3(delta_ms);
          out.scattering.At(i, j, k) =
              H3(out.scattering.At(i, j, k) + delta_ms * (1.0 / RayleighPhaseFunction(nu)));
        }
      }
    }, SCATTERING_TEXTURE_DEPTH, num_threads);
  }

  return out;
}

// ---- RGBA32F output --------------------------------------------------------

std::vector<float> PrecomputedTextures::TransmittanceRGBA() const {
  std::vector<float> v(size_t(transmittance.w) * transmittance.h * 4, 0.f);
  for (size_t p = 0; p < transmittance.data.size(); ++p) {
    const dvec3& c = transmittance.data[p];
    v[p * 4 + 0] = float(c.x);
    v[p * 4 + 1] = float(c.y);
    v[p * 4 + 2] = float(c.z);
    v[p * 4 + 3] = 0.f;  // GPU left transmittance alpha at 0.
  }
  return v;
}

std::vector<float> PrecomputedTextures::ScatteringRGBA(bool combined) const {
  std::vector<float> v(size_t(scattering.w) * scattering.h * scattering.d * 4, 0.f);
  for (size_t p = 0; p < scattering.data.size(); ++p) {
    const dvec3& c = scattering.data[p];
    v[p * 4 + 0] = float(c.x);
    v[p * 4 + 1] = float(c.y);
    v[p * 4 + 2] = float(c.z);
    // Combined mode packs the red channel of single Mie scattering into alpha.
    v[p * 4 + 3] = combined ? float(single_mie.data[p].x) : 0.f;
  }
  return v;
}

std::vector<float> PrecomputedTextures::SingleMieRGBA() const {
  std::vector<float> v(size_t(single_mie.w) * single_mie.h * single_mie.d * 4, 0.f);
  for (size_t p = 0; p < single_mie.data.size(); ++p) {
    const dvec3& c = single_mie.data[p];
    v[p * 4 + 0] = float(c.x);
    v[p * 4 + 1] = float(c.y);
    v[p * 4 + 2] = float(c.z);
    v[p * 4 + 3] = 0.f;
  }
  return v;
}

std::vector<float> PrecomputedTextures::IrradianceRGBA() const {
  std::vector<float> v(size_t(irradiance.w) * irradiance.h * 4, 0.f);
  for (size_t p = 0; p < irradiance.data.size(); ++p) {
    const dvec3& c = irradiance.data[p];
    v[p * 4 + 0] = float(c.x);
    v[p * 4 + 1] = float(c.y);
    v[p * 4 + 2] = float(c.z);
    v[p * 4 + 3] = 0.f;
  }
  return v;
}

}  // namespace bruneton
