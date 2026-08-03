// hillaireatmosphererenderer.cpp
//
// Copyright (C) 2001-present, Celestia Development Team
//
// Implementation of Sebastien Hillaire's 2020 sky/atmosphere technique.
// See hillaireatmosphererenderer.h for an overview.
//
// The scattering model, LUT parameterisations and ray-march integration are
// ported to GLSL from the reference WebGPU implementation by Lukas Herzberger
// (https://github.com/JolifantoBambla/webgpu-sky-atmosphere, MIT) which itself
// follows Hillaire 2020 and the Unreal Engine sky atmosphere (Epic Games, MIT).
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "hillaireatmosphererenderer.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <celcompat/numbers.h>
#include <celengine/atmosphere.h>
#include <celengine/framebuffer.h>
#include <celengine/glsupport.h>
#include <celengine/lightenv.h>
#include <celengine/render.h>
#include <celengine/renderinfo.h>
#include <celengine/shadermanager.h>
#include <celmath/frustum.h>
#include <celmath/mathlib.h>

namespace celestia::render
{

namespace
{

constexpr int TransmittanceLutWidth  = 256;
constexpr int TransmittanceLutHeight = 64;
constexpr int MultiScatterLutSize    = 32;

// GLSL version / precision header, chosen at build time for the active GL API.
const char* shaderHeader()
{
#ifdef GL_ES
    return "#version 300 es\n"
           "precision highp float;\n"
           "precision highp int;\n"
           "precision highp sampler2D;\n";
#else
    return "#version 330\n";
#endif
}

// Shared GLSL: atmosphere uniforms, medium model, phase functions, ray-sphere
// intersection and the transmittance-LUT parameterisation. Included in every
// fragment shader.
const char* commonSrc = R"glsl(
const float PI = 3.14159265358979323846;
const float ISOTROPIC_PHASE = 1.0 / (4.0 * PI);
const float PLANET_RADIUS_OFFSET = 0.01;
const float T_MAX_MAX = 9000000.0;
const float MULTI_SCATTER_RES = 32.0;

uniform float atm_bottom_radius;
uniform float atm_top_radius;
uniform vec3  atm_rayleigh_scattering;
uniform float atm_rayleigh_density_exp_scale;
uniform vec3  atm_mie_scattering;
uniform vec3  atm_mie_extinction;
uniform float atm_mie_density_exp_scale;
uniform float atm_mie_phase_g;
uniform vec3  atm_absorption_extinction;
uniform vec3  atm_ground_albedo;
uniform float atm_multi_scattering_factor;

struct MediumSample
{
    vec3 scattering;
    vec3 extinction;
    vec3 mie_scattering;
    vec3 rayleigh_scattering;
};

// height is metres... no, kilometres above the ground (planet surface).
MediumSample sampleMedium(float height)
{
    float mie_density = exp(atm_mie_density_exp_scale * height);
    float rayleigh_density = exp(atm_rayleigh_density_exp_scale * height);
    // Absorption (ozone) is tied to the Rayleigh density profile here since
    // Celestia atmospheres do not provide a separate ozone tent function.
    float absorption_density = rayleigh_density;

    MediumSample s;
    s.mie_scattering = mie_density * atm_mie_scattering;
    s.rayleigh_scattering = rayleigh_density * atm_rayleigh_scattering;
    s.scattering = s.mie_scattering + s.rayleigh_scattering;
    s.extinction = mie_density * atm_mie_extinction
                 + s.rayleigh_scattering
                 + absorption_density * atm_absorption_extinction;
    return s;
}

vec3 sampleMediumExtinction(float height)
{
    float mie_density = exp(atm_mie_density_exp_scale * height);
    float rayleigh_density = exp(atm_rayleigh_density_exp_scale * height);
    float absorption_density = rayleigh_density;
    return mie_density * atm_mie_extinction
         + rayleigh_density * atm_rayleigh_scattering
         + absorption_density * atm_absorption_extinction;
}

float rayleighPhase(float cos_theta)
{
    return (3.0 / (16.0 * PI)) * (1.0 + cos_theta * cos_theta);
}

float cornetteShanksPhase(float cos_theta, float g)
{
    float g2 = g * g;
    float k = 3.0 / (8.0 * PI) * (1.0 - g2) / (2.0 + g2);
    // note: forward scattering toward the light
    return k * (1.0 + cos_theta * cos_theta) / pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5);
}

// Positive real root of the quadratic, or -1 if none.
float solveQuadratic(float a, float b, float c)
{
    float delta = b * b - 4.0 * a * c;
    if (delta < 0.0 || a == 0.0)
        return -1.0;
    float sd = sqrt(delta);
    float s0 = (-b - sd) / (2.0 * a);
    float s1 = (-b + sd) / (2.0 * a);
    if (s0 < 0.0 && s1 < 0.0)
        return -1.0;
    if (s0 < 0.0)
        return max(0.0, s1);
    else if (s1 < 0.0)
        return max(0.0, s0);
    return max(0.0, min(s0, s1));
}

float raySphere(vec3 o, vec3 d, float r)
{
    return solveQuadratic(dot(d, d), 2.0 * dot(d, o), dot(o, o) - r * r);
}

bool rayIntersectsSphere(vec3 o, vec3 d, float r)
{
    float delta = (2.0 * dot(d, o)) * (2.0 * dot(d, o)) - 4.0 * dot(d, d) * (dot(o, o) - r * r);
    if (delta < 0.0)
        return false;
    float sd = sqrt(delta);
    float a2 = 2.0 * dot(d, d);
    return ((-2.0 * dot(d, o) - sd) / a2) >= 0.0 || ((-2.0 * dot(d, o) + sd) / a2) >= 0.0;
}

// Distance to the first atmosphere/ground boundary. Returns false when the ray
// never enters the atmosphere.
bool findAtmosphereTMax(out float t_max, out float t_bottom, vec3 o, vec3 d)
{
    t_bottom = raySphere(o, d, atm_bottom_radius);
    float t_top = raySphere(o, d, atm_top_radius);
    if (t_bottom < 0.0)
    {
        if (t_top < 0.0) { t_max = 0.0; return false; }
        t_max = t_top;
    }
    else
    {
        if (t_top > 0.0) t_max = min(t_top, t_bottom);
        else             t_max = t_bottom;
    }
    return true;
}

float planetShadow(vec3 o, vec3 d)
{
    return rayIntersectsSphere(o, d, atm_bottom_radius) ? 0.0 : 1.0;
}

vec2 transmittanceLutUv(float view_height, float cos_view_zenith)
{
    float height_sq = view_height * view_height;
    float bottom_sq = atm_bottom_radius * atm_bottom_radius;
    float top_sq = atm_top_radius * atm_top_radius;
    float h = sqrt(max(0.0, top_sq - bottom_sq));
    float rho = sqrt(max(0.0, height_sq - bottom_sq));
    float discriminant = height_sq * (cos_view_zenith * cos_view_zenith - 1.0) + top_sq;
    float d = max(0.0, (-view_height * cos_view_zenith + sqrt(max(discriminant, 0.0))));
    float d_min = atm_top_radius - view_height;
    float d_max = rho + h;
    float x_mu = (d - d_min) / (d_max - d_min);
    float x_r = rho / h;
    return vec2(x_mu, x_r);
}
)glsl";

// -- Vertex shaders -------------------------------------------------------

const char* lutVertexSrc = R"glsl(
in vec4 in_Position;
out vec2 v_uv;
void main()
{
    gl_Position = in_Position;
    v_uv = in_Position.xy * 0.5 + 0.5;
}
)glsl";

const char* skyVertexSrc = R"glsl(
in vec4 in_Position;
uniform int  use_shell;
uniform float shell_radius;
uniform vec3 camera;
uniform mat4 clip_from_model;
uniform mat4 model_from_view;
uniform mat4 view_from_clip;
out vec3 view_ray;
void main()
{
    if (use_shell != 0)
    {
        vec3 model_position = in_Position.xyz * shell_radius;
        view_ray = model_position - camera;
        gl_Position = clip_from_model * vec4(model_position, 1.0);
    }
    else
    {
        vec3 vr_view = (view_from_clip * in_Position).xyz;
        view_ray = (model_from_view * vec4(vr_view, 0.0)).xyz;
        // Emit at the far plane (ndc z = 1) so opaque terrain, which is nearer,
        // depth-masks the quad. Only true sky/space pixels (cleared depth) pass,
        // which fixes the grazing-horizon black seam and skips the march on
        // terrain (early-Z) for a perf win near the ground.
        gl_Position = vec4(in_Position.xy, in_Position.w, in_Position.w);
    }
}
)glsl";

// -- Transmittance LUT fragment ------------------------------------------

const char* transmittanceFragSrc = R"glsl(
in vec2 v_uv;
out vec4 frag;

vec2 uvToTransmittanceParams(vec2 uv)
{
    float bottom_sq = atm_bottom_radius * atm_bottom_radius;
    float h_sq = atm_top_radius * atm_top_radius - bottom_sq;
    float h = sqrt(h_sq);
    float rho = h * uv.y;
    float view_height = sqrt(rho * rho + bottom_sq);
    float d_min = atm_top_radius - view_height;
    float d_max = rho + h;
    float d = d_min + uv.x * (d_max - d_min);
    float cos_view_zenith = 1.0;
    if (d != 0.0)
        cos_view_zenith = clamp((h_sq - rho * rho - d * d) / (2.0 * view_height * d), -1.0, 1.0);
    return vec2(view_height, cos_view_zenith);
}

void main()
{
    vec2 p = uvToTransmittanceParams(v_uv);
    float view_height = p.x;
    float cos_vz = p.y;
    vec2 o = vec2(0.0, view_height);
    vec2 dir = vec2(sqrt(max(1.0 - cos_vz * cos_vz, 0.0)), cos_vz);

    // 2D ray/circle intersection to find the atmosphere exit distance.
    float a = dot(dir, dir);
    float b = 2.0 * dot(dir, o);
    float t_bottom = solveQuadratic(a, b, dot(o, o) - atm_bottom_radius * atm_bottom_radius);
    float t_top = solveQuadratic(a, b, dot(o, o) - atm_top_radius * atm_top_radius);
    float t_max;
    if (t_bottom < 0.0) t_max = (t_top < 0.0) ? 0.0 : t_top;
    else                t_max = (t_top > 0.0) ? min(t_top, t_bottom) : 0.0;
    t_max = min(t_max, T_MAX_MAX);

    const float SAMPLES = 40.0;
    float dt = t_max / SAMPLES;
    vec3 optical_depth = vec3(0.0);
    float t = 0.0;
    for (float s = 0.0; s < SAMPLES; s += 1.0)
    {
        float t_new = (s + 0.3) * dt;
        float dt_exact = t_new - t;
        t = t_new;
        float height = length(o + t * dir) - atm_bottom_radius;
        optical_depth += sampleMediumExtinction(height) * dt_exact;
    }
    frag = vec4(exp(-optical_depth), 1.0);
}
)glsl";

// -- Multiple-scattering LUT fragment ------------------------------------

const char* multiscatterFragSrc = R"glsl(
in vec2 v_uv;
out vec4 frag;
uniform sampler2D transmittance_lut;

float fromUnitToSubUv(float u, float res)
{
    return (u + 0.5 / res) * (res / (res + 1.0));
}
float fromSubUvToUnit(float u, float res)
{
    return (u - 0.5 / res) * (res / (res - 1.0));
}

vec3 transmittanceToSun(vec3 sun_dir, vec3 zenith, float sample_height)
{
    float cos_sun_zenith = dot(sun_dir, zenith);
    return texture(transmittance_lut, transmittanceLutUv(sample_height, cos_sun_zenith)).rgb;
}

vec3 sampleDirection(float i)
{
    const float GOLD = 1.61803398875;
    const float N = 64.0;
    float theta = 2.0 * PI * i / GOLD;
    float phi = acos(1.0 - 2.0 * (i + 0.5) / N);
    float cp = cos(phi), sp = sin(phi);
    return vec3(cos(theta) * sp, sin(theta) * sp, cp);
}

void main()
{
    vec2 uv = vec2(fromSubUvToUnit(v_uv.x, MULTI_SCATTER_RES),
                   fromSubUvToUnit(v_uv.y, MULTI_SCATTER_RES));
    float cos_sun_zenith = uv.x * 2.0 - 1.0;
    vec3 sun_dir = normalize(vec3(0.0, sqrt(clamp(1.0 - cos_sun_zenith * cos_sun_zenith, 0.0, 1.0)), cos_sun_zenith));
    float view_height = atm_bottom_radius + clamp(uv.y + PLANET_RADIUS_OFFSET, 0.0, 1.0) *
                        (atm_top_radius - atm_bottom_radius - PLANET_RADIUS_OFFSET);
    vec3 world_pos = vec3(0.0, 0.0, view_height);

    const float DIRS = 64.0;
    const float SAMPLES = 20.0;
    vec3 lum_total = vec3(0.0);
    vec3 ms_total = vec3(0.0);

    for (float di = 0.0; di < DIRS; di += 1.0)
    {
        vec3 world_dir = sampleDirection(di);
        float t_max, t_bottom;
        if (!findAtmosphereTMax(t_max, t_bottom, world_pos, world_dir))
            continue;
        t_max = min(t_max, T_MAX_MAX);
        float dt = t_max / SAMPLES;
        vec3 throughput = vec3(1.0);
        vec3 lum = vec3(0.0);
        vec3 ms = vec3(0.0);
        float t = 0.0;
        for (float s = 0.0; s < SAMPLES; s += 1.0)
        {
            float t_new = (s + 0.3) * dt;
            float dt_exact = t_new - t;
            t = t_new;
            vec3 sp = world_pos + t * world_dir;
            float sh = length(sp);
            vec3 zenith = sp / sh;
            vec3 tr_sun = transmittanceToSun(sun_dir, zenith, sh);
            MediumSample md = sampleMedium(sh - atm_bottom_radius);
            vec3 sample_transmittance = exp(-md.extinction * dt_exact);
            float pshadow = planetShadow(sp, sun_dir);
            vec3 scattered = pshadow * tr_sun * md.scattering * ISOTROPIC_PHASE;
            ms += throughput * (md.scattering - md.scattering * sample_transmittance) / md.extinction;
            lum += throughput * (scattered - scattered * sample_transmittance) / md.extinction;
            throughput *= sample_transmittance;
        }
        // ground bounce
        if (t_max == t_bottom && t_bottom > 0.0)
        {
            vec3 sp = world_pos + t_bottom * world_dir;
            float sh = length(sp);
            vec3 zenith = sp / sh;
            vec3 tr_sun = transmittanceToSun(sun_dir, zenith, sh);
            float ndotl = clamp(dot(zenith, sun_dir), 0.0, 1.0);
            lum += tr_sun * throughput * ndotl * atm_ground_albedo / PI;
        }
        lum_total += lum / DIRS;
        ms_total += ms / DIRS;
    }

    vec3 L = lum_total * (1.0 / (1.0 - ms_total));
    frag = vec4(atm_multi_scattering_factor * L, 1.0);
}
)glsl";

// -- Sky fragment (ray march) --------------------------------------------

const char* skyFragSrc = R"glsl(
in vec3 view_ray;
out vec4 frag;
uniform sampler2D transmittance_lut;
uniform sampler2D multiscatter_lut;
uniform vec3 camera;
uniform int  render_mode;      // 0 = view transmittance, 1 = inscattered luminance
uniform int  num_lights;
uniform vec3 light_dir[2];
uniform vec3 light_illuminance[2];
uniform float luminance_scale;
uniform float fade;           // 0..1 smooth ramp-in as the atmosphere grows on screen
uniform int  use_shell;       // 1 = shell geometry (planet disk visible), 0 = near-ground quad

float fromUnitToSubUv(float u, float res)
{
    return (u + 0.5 / res) * (res / (res + 1.0));
}

vec3 getMultipleScattering(vec3 world_pos, float cos_view_zenith)
{
    vec2 uv = clamp(vec2(cos_view_zenith * 0.5 + 0.5,
                         (length(world_pos) - atm_bottom_radius) /
                         (atm_top_radius - atm_bottom_radius)), 0.0, 1.0);
    uv = vec2(fromUnitToSubUv(uv.x, MULTI_SCATTER_RES),
              fromUnitToSubUv(uv.y, MULTI_SCATTER_RES));
    return texture(multiscatter_lut, uv).rgb;
}

void main()
{
    vec3 dir = normalize(view_ray);
    vec3 pos = camera;

    float view_height = length(pos);
    // If outside the atmosphere, advance the ray to the atmosphere top.
    if (view_height > atm_top_radius)
    {
        float t_top = raySphere(pos, dir, atm_top_radius * 0.9999);
        if (t_top < 0.0)
        {
            // Ray misses the atmosphere entirely.
            frag = (render_mode == 0) ? vec4(1.0) : vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        pos += dir * t_top;
    }
    // Determine the integration limit. The two rendering paths differ:
    //   * Shell path (planet viewed from space): rays that strike the planet
    //     must truncate at the ground, otherwise the chord integrates straight
    //     through the globe and pass 0 multiplies the surface to black.
    //   * Quad path (near ground): opaque terrain is depth-masked in hardware,
    //     so only true sky/space pixels reach this shader. Integrate the full
    //     sky path to the atmosphere exit; truncating at the analytic ground
    //     here would recreate the grazing-horizon inscatter collapse (black
    //     seam) in the thin gap where the terrain mesh sags inside the sphere.
    float t_max;
    if (use_shell != 0)
    {
        float t_bottom;
        if (!findAtmosphereTMax(t_max, t_bottom, pos, dir))
        {
            frag = (render_mode == 0) ? vec4(1.0) : vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        t_max = min(t_max, T_MAX_MAX);
    }
    else
    {
        float t_top = raySphere(pos, dir, atm_top_radius);   // forward atmosphere exit
        if (t_top <= 0.0)
        {
            frag = (render_mode == 0) ? vec4(1.0) : vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        t_max = min(t_top, T_MAX_MAX);
    }

    float sample_count = mix(4.0, 32.0, clamp(t_max / 200.0, 0.0, 1.0));
    float sample_count_floored = floor(sample_count);
    float inv_sample_count_floored = 1.0 / sample_count_floored;
    float t_max_floored = t_max * sample_count_floored / sample_count;

    // Precompute phase terms per light.
    vec3 luminance = vec3(0.0);
    vec3 transmittance = vec3(1.0);

    float t = 0.0;
    for (float s = 0.0; s < sample_count; s += 1.0)
    {
        float t0 = s * inv_sample_count_floored;
        float t1 = (s + 1.0) * inv_sample_count_floored;
        t0 = (t0 * t0) * t_max_floored;
        t1 = t1 * t1;
        t1 = (t1 > 1.0) ? t_max : t_max_floored * t1;
        float dt = t1 - t0;
        t = t0 + dt * 0.3;

        vec3 sp = pos + t * dir;
        float sh = length(sp);
        vec3 zenith = sp / sh;
        MediumSample md = sampleMedium(sh - atm_bottom_radius);
        vec3 sample_transmittance = exp(-md.extinction * dt);

        // Pass 0 (render_mode 0) only needs the accumulated view transmittance
        // (extinction integral); it never uses luminance. Skip the expensive
        // per-light scattering + LUT sampling in that pass.
        if (render_mode != 0)
        {
            vec3 scattered = vec3(0.0);
            for (int li = 0; li < num_lights; ++li)
            {
                vec3 L = light_dir[li];
                float cos_theta = dot(L, dir);
                float mie_ph = cornetteShanksPhase(cos_theta, atm_mie_phase_g);
                float ray_ph = rayleighPhase(cos_theta);
                float cos_sun_zenith = dot(L, zenith);
                vec3 tr_sun = texture(transmittance_lut, transmittanceLutUv(sh, cos_sun_zenith)).rgb;
                vec3 phase_scatter = md.mie_scattering * mie_ph + md.rayleigh_scattering * ray_ph;
                vec3 ms = getMultipleScattering(sp, cos_sun_zenith);
                float pshadow = planetShadow(sp, L);
                scattered += light_illuminance[li] *
                    (pshadow * tr_sun * phase_scatter + ms * md.scattering);
            }

            vec3 integ = (scattered - scattered * sample_transmittance) / md.extinction;
            luminance += transmittance * integ;
        }
        transmittance *= sample_transmittance;
    }

    // fade smoothly ramps the atmosphere in as it grows past ~2px thick,
    // matching the legacy renderer and avoiding a pop-on at the fade threshold.
    // Pass 0 blends transmittance toward 1 (no extinction); pass 1 scales the
    // additive inscattering.
    if (render_mode == 0)
        frag = vec4(mix(vec3(1.0), clamp(transmittance, 0.0, 1.0), fade), 1.0);
    else
        frag = vec4(max(luminance * luminance_scale, vec3(0.0)) * fade, 1.0);
}
)glsl";

GLProgram
buildProgram(const char* vertexSrc, const char* fragmentSrc, bool withCommon)
{
    GLShaderStatus status;
    GLProgramBuilder builder = GLProgramBuilder::create(status);
    if (status != GLShaderStatus::OK)
        return GLProgram{};

    std::string vs = std::string(shaderHeader()) + vertexSrc;
    std::string fs = std::string(shaderHeader());
    if (withCommon)
        fs += commonSrc;
    fs += fragmentSrc;

    if (auto v = GLVertexShader::create(vs, status); status == GLShaderStatus::OK)
        builder.attach(std::move(v));
    else
        return GLProgram{};

    if (auto f = GLFragmentShader::create(fs, status); status == GLShaderStatus::OK)
        builder.attach(std::move(f));
    else
        return GLProgram{};

    builder.bindAttribute(CelestiaGLProgram::VertexCoordAttributeIndex, "in_Position");

    GLProgram program = builder.link(status);
    if (status != GLShaderStatus::OK)
        return GLProgram{};
    return program;
}

} // anonymous namespace

HillaireAtmosphereRenderer::HillaireAtmosphereRenderer(Renderer &renderer) :
    m_renderer(renderer)
{
}

HillaireAtmosphereRenderer::~HillaireAtmosphereRenderer() = default;

bool
HillaireAtmosphereRenderer::Params::approxEqual(const Params &o) const
{
    auto eq = [](float a, float b) { return std::abs(a - b) < 1.0e-4f * (1.0f + std::abs(a)); };
    auto veq = [&](const Eigen::Vector3f &a, const Eigen::Vector3f &b)
    { return eq(a.x(), b.x()) && eq(a.y(), b.y()) && eq(a.z(), b.z()); };
    return eq(bottomRadius, o.bottomRadius) && eq(topRadius, o.topRadius) &&
           veq(rayleighScattering, o.rayleighScattering) &&
           eq(rayleighDensityExpScale, o.rayleighDensityExpScale) &&
           veq(mieScattering, o.mieScattering) && veq(mieExtinction, o.mieExtinction) &&
           eq(mieDensityExpScale, o.mieDensityExpScale) && eq(miePhaseG, o.miePhaseG) &&
           veq(absorptionExtinction, o.absorptionExtinction) &&
           veq(groundAlbedo, o.groundAlbedo) &&
           eq(multiScatteringFactor, o.multiScatteringFactor);
}

void
HillaireAtmosphereRenderer::initGL()
{
    if (m_initialized)
        return;
    m_initialized = true;
    m_programsValid = compilePrograms();
    if (m_programsValid)
        buildGeometry();
}

bool
HillaireAtmosphereRenderer::compilePrograms()
{
    m_transmittanceProgram = buildProgram(lutVertexSrc, transmittanceFragSrc, true);
    m_multiscatterProgram = buildProgram(lutVertexSrc, multiscatterFragSrc, true);
    m_skyProgram = buildProgram(skyVertexSrc, skyFragSrc, true);
    return m_transmittanceProgram.isValid() && m_multiscatterProgram.isValid() &&
           m_skyProgram.isValid();
}

void
HillaireAtmosphereRenderer::buildGeometry()
{
    // Full-screen quad (clip-space) for LUT passes and the in-atmosphere sky.
    constexpr std::array<std::array<float, 4>, 4> quad
    {{
        {{ -1.0f, -1.0f, 0.0f, 1.0f }},
        {{  1.0f, -1.0f, 0.0f, 1.0f }},
        {{ -1.0f,  1.0f, 0.0f, 1.0f }},
        {{  1.0f,  1.0f, 0.0f, 1.0f }},
    }};
    m_quadBo = gl::Buffer(gl::Buffer::TargetHint::Array);
    m_quadBo.setData(quad);
    m_quadVo = gl::VertexObject(gl::VertexObject::Primitive::TriangleStrip);
    m_quadVo.addVertexBuffer(m_quadBo, CelestiaGLProgram::VertexCoordAttributeIndex, 4,
                             gl::VertexObject::DataType::Float, false, sizeof(quad[0]), 0);
    m_quadVo.setCount(static_cast<GLsizei>(quad.size()));

    // Unit sphere shell (scaled by top radius in the vertex shader). Wound so
    // the exterior is front-facing (CCW); back-face culling keeps the near
    // hemisphere - the atmosphere entry surface.
    constexpr int stacks = 32;
    constexpr int slices = 64;
    const float pi = celestia::numbers::pi_v<float>;
    std::vector<std::array<float, 3>> verts;
    verts.reserve((stacks + 1) * (slices + 1));
    for (int i = 0; i <= stacks; ++i)
    {
        float phi = static_cast<float>(i) / stacks * pi;
        float y = std::cos(phi);
        float r = std::sin(phi);
        for (int j = 0; j <= slices; ++j)
        {
            float theta = static_cast<float>(j) / slices * 2.0f * pi;
            verts.push_back({ r * std::cos(theta), y, r * std::sin(theta) });
        }
    }
    std::vector<unsigned short> indices;
    indices.reserve(stacks * slices * 6);
    const int stride = slices + 1;
    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            auto a = static_cast<unsigned short>(i * stride + j);
            auto b = static_cast<unsigned short>(a + stride);
            auto c = static_cast<unsigned short>(a + 1);
            auto d = static_cast<unsigned short>(b + 1);
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(c); indices.push_back(d); indices.push_back(b);
        }
    }
    m_shellBo = gl::Buffer(gl::Buffer::TargetHint::Array);
    m_shellBo.setData(verts);
    m_shellVo = gl::VertexObject(gl::VertexObject::Primitive::Triangles);
    m_shellVo.addVertexBuffer(m_shellBo, CelestiaGLProgram::VertexCoordAttributeIndex, 3,
                              gl::VertexObject::DataType::Float, false, sizeof(verts[0]), 0);
    gl::Buffer ib(gl::Buffer::TargetHint::ElementArray);
    ib.setData(indices);
    m_shellVo.setIndexBuffer(std::move(ib), 0, gl::VertexObject::IndexType::UnsignedShort);
    m_shellVo.setCount(static_cast<GLsizei>(indices.size()));
}

HillaireAtmosphereRenderer::Params
HillaireAtmosphereRenderer::makeParams(const Atmosphere &atmosphere, float radius) const
{
    Params p;
    p.bottomRadius = radius;
    p.topRadius = radius + (atmosphere.height > 0.0f ? atmosphere.height : 100.0f);

    // Celestia stores physical per-km coefficients; a single scale height
    // (mieScaleHeight) drives both media in the legacy data.
    float scaleHeight = atmosphere.mieScaleHeight > 0.0f ? atmosphere.mieScaleHeight : 8.0f;
    float rayleighScaleHeight = atmosphere.rayleighScaleHeight > 0.0f
                                    ? atmosphere.rayleighScaleHeight : scaleHeight;

    p.rayleighScattering = atmosphere.rayleighCoeff;
    p.rayleighDensityExpScale = -1.0f / rayleighScaleHeight;

    Eigen::Vector3f mie = Eigen::Vector3f::Constant(atmosphere.mieCoeff);
    p.mieScattering = mie;
    p.mieExtinction = mie * 1.11f;  // Mie also absorbs (single-scatter albedo ~0.9)
    p.mieDensityExpScale = -1.0f / scaleHeight;
    p.miePhaseG = atmosphere.miePhaseAsymmetry;

    p.absorptionExtinction = atmosphere.absorptionCoeff;
    p.groundAlbedo = Eigen::Vector3f::Constant(0.3f);
    p.multiScatteringFactor = 1.0f;
    return p;
}

void
HillaireAtmosphereRenderer::setAtmosphereUniforms(GLuint programId, const Params &p) const
{
    FloatShaderParameter(programId, "atm_bottom_radius") = p.bottomRadius;
    FloatShaderParameter(programId, "atm_top_radius") = p.topRadius;
    Vec3ShaderParameter(programId, "atm_rayleigh_scattering") = p.rayleighScattering;
    FloatShaderParameter(programId, "atm_rayleigh_density_exp_scale") = p.rayleighDensityExpScale;
    Vec3ShaderParameter(programId, "atm_mie_scattering") = p.mieScattering;
    Vec3ShaderParameter(programId, "atm_mie_extinction") = p.mieExtinction;
    FloatShaderParameter(programId, "atm_mie_density_exp_scale") = p.mieDensityExpScale;
    FloatShaderParameter(programId, "atm_mie_phase_g") = p.miePhaseG;
    Vec3ShaderParameter(programId, "atm_absorption_extinction") = p.absorptionExtinction;
    Vec3ShaderParameter(programId, "atm_ground_albedo") = p.groundAlbedo;
    FloatShaderParameter(programId, "atm_multi_scattering_factor") = p.multiScatteringFactor;
}

void
HillaireAtmosphereRenderer::ensureLuts(const Params &params)
{
    if (m_lutValid && m_lutParams.approxEqual(params))
        return;

    if (m_transmittanceLut == nullptr)
        m_transmittanceLut = std::make_unique<FramebufferObject>(
            TransmittanceLutWidth, TransmittanceLutHeight,
            FramebufferObject::Attachment::Color, 1, true);
    if (m_multiscatterLut == nullptr)
        m_multiscatterLut = std::make_unique<FramebufferObject>(
            MultiScatterLutSize, MultiScatterLutSize,
            FramebufferObject::Attachment::Color, 1, true);

    GLint oldFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
    std::array<GLint, 4> oldViewport{};
    glGetIntegerv(GL_VIEWPORT, oldViewport.data());

    Renderer::PipelineState ps;
    ps.depthTest = false;
    ps.depthMask = false;
    ps.blending = false;
    m_renderer.setPipelineState(ps);

    // Transmittance LUT.
    m_transmittanceLut->bind();
    glViewport(0, 0, TransmittanceLutWidth, TransmittanceLutHeight);
    m_transmittanceProgram.use();
    setAtmosphereUniforms(m_transmittanceProgram.getID(), params);
    m_quadVo.draw();

    // Multiple-scattering LUT (samples the transmittance LUT).
    m_multiscatterLut->bind();
    glViewport(0, 0, MultiScatterLutSize, MultiScatterLutSize);
    m_multiscatterProgram.use();
    setAtmosphereUniforms(m_multiscatterProgram.getID(), params);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_transmittanceLut->colorTexture());
    IntegerShaderParameter(m_multiscatterProgram.getID(), "transmittance_lut") = 0;
    m_quadVo.draw();

    m_transmittanceLut->unbind(oldFbo);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);

    m_lutParams = params;
    m_lutValid = true;
}

bool
HillaireAtmosphereRenderer::render(
    const RenderInfo         &ri,
    const Atmosphere         &atmosphere,
    const LightingState      &ls,
    const Eigen::Quaternionf &planetOrientation,
    const Eigen::Vector3f    &semiAxes,
    float                     radius,
    const math::Frustum      &frustum,
    const Matrices           &m,
    float                     fade)
{
    if (!m_initialized)
        initGL();
    if (!m_programsValid || ls.nLights == 0)
        return false;

    Params params = makeParams(atmosphere, radius);
    ensureLuts(params);

    const GLuint programId = m_skyProgram.getID();
    m_skyProgram.use();
    setAtmosphereUniforms(programId, params);

    // Camera position in km with the planet centred at origin. ri.eyePos_obj is
    // in units of the planet's mean radius, so scaling by the bottom radius (km)
    // maps it into the physical, spherical atmosphere space.
    Eigen::Vector3f bodySemiAxes = semiAxes;
    Eigen::Vector3f camera = ri.eyePos_obj * params.bottomRadius;
    Vec3ShaderParameter(programId, "camera") = camera;

    float cameraRadius = camera.norm();

    // Lights: up to two directional sources, directions in object space. Cull
    // sources whose irradiance is negligible relative to the brightest one; a
    // faint secondary star (e.g. Earth's ~0.0007 irradiance companion) adds no
    // visible scattering but doubles the per-sample LUT sampling cost, which
    // matters for the near-ground full-screen march.
    unsigned int candidateLights = std::min<unsigned int>(ls.nLights, 2u);
    float maxIrr = 0.0f;
    for (unsigned int i = 0; i < candidateLights; ++i)
        maxIrr = std::max(maxIrr, ls.lights[i].irradiance);
    const float irrCutoff = maxIrr * 0.01f;

    int numLights = 0;
    for (unsigned int i = 0; i < candidateLights; ++i)
    {
        float irr = ls.lights[i].irradiance;
        if (irr < irrCutoff)
            continue;
        Eigen::Vector3f d = ls.lights[i].direction_obj.normalized();
        Color c = ls.lights[i].color;
        // color is the light's chromaticity; irradiance is its magnitude. Scale
        // by irradiance so faint secondary stars don't light the atmosphere as
        // brightly as the primary sun (which would paint a spurious day-side).
        Eigen::Vector3f illum(c.red() * irr, c.green() * irr, c.blue() * irr);
        std::string di = "light_dir[" + std::to_string(numLights) + "]";
        std::string ii = "light_illuminance[" + std::to_string(numLights) + "]";
        Vec3ShaderParameter(programId, di.c_str()) = d;
        Vec3ShaderParameter(programId, ii.c_str()) = illum;
        ++numLights;
    }
    IntegerShaderParameter(programId, "num_lights") = numLights;

    static const float lumScale = []() {
        if (const char* e = std::getenv("CEL_ATM_LUM"))
            return static_cast<float>(std::atof(e));
        return 6.0f;
    }();
    FloatShaderParameter(programId, "luminance_scale") = lumScale;
    FloatShaderParameter(programId, "fade") = fade;

    // LUT textures.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_transmittanceLut->colorTexture());
    IntegerShaderParameter(programId, "transmittance_lut") = 0;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_multiscatterLut->colorTexture());
    IntegerShaderParameter(programId, "multiscatter_lut") = 1;
    glActiveTexture(GL_TEXTURE0);

    // Decide shell vs full-screen quad based on camera clearance above the
    // atmosphere top (relative margin avoids near-plane clipping of the shell).
    const bool useShell = (cameraRadius - params.topRadius) > params.bottomRadius * 5.0e-4f;
    IntegerShaderParameter(programId, "use_shell") = useShell ? 1 : 0;
    FloatShaderParameter(programId, "shell_radius") = params.topRadius;

    // Matrices.
    Eigen::Matrix4f modelFromView = Eigen::Matrix4f::Identity();
    modelFromView.topLeftCorner<3, 3>() =
        (params.bottomRadius * bodySemiAxes.cwiseInverse()).asDiagonal() *
        ri.orientation.conjugate().toRotationMatrix();
    Mat4ShaderParameter(programId, "model_from_view") = modelFromView;
    Mat4ShaderParameter(programId, "view_from_clip") = m.projection->inverse();

    Eigen::Matrix4f modelScale = Eigen::Matrix4f::Identity();
    float invBottomRadius = 1.0f / params.bottomRadius;
    modelScale(0, 0) = invBottomRadius;
    modelScale(1, 1) = invBottomRadius;
    modelScale(2, 2) = invBottomRadius;
    Mat4ShaderParameter(programId, "clip_from_model") =
        (*m.projection) * (*m.modelview) * modelScale;

    glFrontFace(GL_CCW);

    Renderer::PipelineState ps;
    ps.blending = true;
    // Depth-test both paths. The shell is real geometry; the quad is emitted at
    // the far plane so nearer terrain masks it (fixes the grazing-horizon seam
    // and early-Z rejects terrain for a perf win). depthMask stays off so the
    // atmosphere never writes depth.
    ps.depthTest = true;
    ps.depthMask = false;

    gl::VertexObject &vo = useShell ? m_shellVo : m_quadVo;

    // Pass 1: multiply the framebuffer by the atmosphere transmittance.
    IntegerShaderParameter(programId, "render_mode") = 0;
    ps.blendFunc = { GL_ZERO, GL_SRC_COLOR };
    m_renderer.setPipelineState(ps);
    vo.draw();

    // Pass 2: add the inscattered luminance.
    IntegerShaderParameter(programId, "render_mode") = 1;
    ps.blendFunc = { GL_ONE, GL_ONE };
    m_renderer.setPipelineState(ps);
    vo.draw();

    glFrontFace(GL_CCW);
    return true;
}

} // namespace celestia::render
