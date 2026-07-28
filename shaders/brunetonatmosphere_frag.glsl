// SPDX-License-Identifier: BSD-3-Clause
// Runtime subset of Eric Bruneton's precomputed atmospheric scattering model.
// This shader intentionally outputs linear HDR values. Celestia applies
// exposure and tone mapping as a post-processing effect.

#ifdef GL_ES
precision highp sampler2D;
precision highp sampler3D;
#endif

#define IN(x) const in x
#define OUT(x) out x
#define assert(x)

#define Length float
#define Area float
#define Number float
#define InverseLength float
#define InverseSolidAngle float
#define IrradianceSpectrum vec3
#define RadianceSpectrum vec3
#define DimensionlessSpectrum vec3
#define Position vec3
#define Direction vec3
#define TransmittanceTexture sampler2D
#define ReducedScatteringTexture sampler3D
#define IrradianceTexture sampler2D

const float PI = 3.14159265358979323846;
const Length m = 1.0;
const Area m2 = 1.0;
const InverseSolidAngle sr = 1.0;
const RadianceSpectrum watt_per_square_meter_per_sr_per_nm = vec3(1.0);

const int TRANSMITTANCE_TEXTURE_WIDTH = 256;
const int TRANSMITTANCE_TEXTURE_HEIGHT = 64;
const int SCATTERING_TEXTURE_R_SIZE = 32;
const int SCATTERING_TEXTURE_MU_SIZE = 256;
const int SCATTERING_TEXTURE_MU_S_SIZE = 32;
const int SCATTERING_TEXTURE_NU_SIZE = 8;
const int IRRADIANCE_TEXTURE_WIDTH = 64;
const int IRRADIANCE_TEXTURE_HEIGHT = 16;
const int MAX_ECLIPSE_SHADOWS = 3;

struct DensityProfileLayer
{
    Length width;
    Number exp_term;
    InverseLength exp_scale;
    InverseLength linear_term;
    Number constant_term;
};

struct AtmosphereParameters
{
    vec3 solar_irradiance;
    float sun_angular_radius;
    Length bottom_radius;
    Length top_radius;
    DensityProfileLayer rayleigh_density0;
    DensityProfileLayer rayleigh_density1;
    vec3 rayleigh_scattering;
    DensityProfileLayer mie_density0;
    DensityProfileLayer mie_density1;
    vec3 mie_scattering;
    vec3 mie_extinction;
    Number mie_phase_function_g;
    DensityProfileLayer absorption_density0;
    DensityProfileLayer absorption_density1;
    vec3 absorption_extinction;
    Number mu_s_min;
};

uniform AtmosphereParameters atmosphere;
uniform sampler2D transmittance_texture;
uniform sampler3D scattering_texture;
uniform sampler3D single_mie_scattering_texture;
uniform sampler2D irradiance_texture;
uniform sampler2D scene_depth_texture;
uniform sampler2D surface_id_texture;
uniform sampler2D depth_partition_texture;
uniform sampler2D cloud_texture;
uniform sampler2D cloud_normal_texture;
uniform int combined_scattering_textures;
uniform int precomputed_luminance;
uniform int manual_float_filtering;
uniform int manual_scattering_filtering;
uniform int depth_partition_count;
uniform int render_clouds;
uniform int render_cloud_normals;
uniform int cloud_normal_texture_dxt5;
uniform int cloud_texture_has_alpha;
uniform float surface_body_id;
uniform vec3 camera;
uniform vec3 earth_center;
uniform vec3 sun_direction;
uniform vec3 sky_spectral_radiance_to_luminance;
uniform vec3 sun_spectral_radiance_to_luminance;
uniform float luminance_scale;
uniform float cloud_radius;
uniform float cloud_texture_offset;
uniform vec2 viewport_size;
uniform vec2 viewport_origin;
uniform int render_mode;
uniform int eclipse_shadow_count;
uniform vec4 eclipse_shadow_tex_gen_s[MAX_ECLIPSE_SHADOWS];
uniform vec4 eclipse_shadow_tex_gen_t[MAX_ECLIPSE_SHADOWS];
uniform float eclipse_shadow_falloff[MAX_ECLIPSE_SHADOWS];
uniform float eclipse_shadow_max_depth[MAX_ECLIPSE_SHADOWS];

in vec3 view_ray;
in vec3 view_ray_view;

Number ClampCosine(Number mu)
{
    return clamp(mu, Number(-1.0), Number(1.0));
}

Length ClampDistance(Length d)
{
    return max(d, 0.0 * m);
}

Length ClampRadius(IN(AtmosphereParameters) parameters, Length r)
{
    return clamp(r, parameters.bottom_radius, parameters.top_radius);
}

Length SafeSqrt(Area a)
{
    return sqrt(max(a, 0.0 * m2));
}

Length DistanceToTopAtmosphereBoundary(
    IN(AtmosphereParameters) parameters,
    Length r,
    Number mu)
{
    Area discriminant =
        r * r * (mu * mu - 1.0) +
        parameters.top_radius * parameters.top_radius;
    return ClampDistance(-r * mu + SafeSqrt(discriminant));
}

bool RayIntersectsGround(
    IN(AtmosphereParameters) parameters,
    Length r,
    Number mu)
{
    return mu < 0.0 &&
        r * r * (mu * mu - 1.0) +
            parameters.bottom_radius * parameters.bottom_radius >=
        0.0 * m2;
}

Number GetTextureCoordFromUnitRange(Number x, int texture_size)
{
    return 0.5 / Number(texture_size) +
        x * (1.0 - 1.0 / Number(texture_size));
}

vec2 GetTransmittanceTextureUvFromRMu(
    IN(AtmosphereParameters) parameters,
    Length r,
    Number mu)
{
    Length H = sqrt(
        parameters.top_radius * parameters.top_radius -
        parameters.bottom_radius * parameters.bottom_radius);
    Length rho = SafeSqrt(
        r * r - parameters.bottom_radius * parameters.bottom_radius);
    Length d = DistanceToTopAtmosphereBoundary(parameters, r, mu);
    Length d_min = parameters.top_radius - r;
    Length d_max = rho + H;
    Number x_mu = (d - d_min) / (d_max - d_min);
    Number x_r = rho / H;
    return vec2(
        GetTextureCoordFromUnitRange(x_mu, TRANSMITTANCE_TEXTURE_WIDTH),
        GetTextureCoordFromUnitRange(x_r, TRANSMITTANCE_TEXTURE_HEIGHT));
}

vec4 SampleFloatTexture2DLinear(sampler2D sampler, vec2 uv)
{
    ivec2 size = textureSize(sampler, 0);
    vec2 position = uv * vec2(size) - vec2(0.5);
    ivec2 lower = ivec2(floor(position));
    vec2 weight = fract(position);
    ivec2 maximum = size - ivec2(1);
    ivec2 p00 = clamp(lower, ivec2(0), maximum);
    ivec2 p10 = clamp(lower + ivec2(1, 0), ivec2(0), maximum);
    ivec2 p01 = clamp(lower + ivec2(0, 1), ivec2(0), maximum);
    ivec2 p11 = clamp(lower + ivec2(1), ivec2(0), maximum);
    vec4 row0 = mix(
        texelFetch(sampler, p00, 0),
        texelFetch(sampler, p10, 0),
        weight.x);
    vec4 row1 = mix(
        texelFetch(sampler, p01, 0),
        texelFetch(sampler, p11, 0),
        weight.x);
    return mix(row0, row1, weight.y);
}

vec4 SampleFloatTexture2D(sampler2D sampler, vec2 uv)
{
    return manual_float_filtering != 0
        ? SampleFloatTexture2DLinear(sampler, uv)
        : texture(sampler, uv);
}

vec4 SampleFloatTexture3DLinear(sampler3D sampler, vec3 uv)
{
    ivec3 size = textureSize(sampler, 0);
    vec3 coordinate = clamp(
        uv * vec3(size) - vec3(0.5),
        vec3(0.0),
        vec3(size - 1));
    ivec3 p0 = ivec3(floor(coordinate));
    ivec3 p1 = min(p0 + ivec3(1), size - ivec3(1));
    vec3 weight = fract(coordinate);
    vec4 z0 = mix(
        mix(texelFetch(sampler, ivec3(p0.x, p0.y, p0.z), 0),
            texelFetch(sampler, ivec3(p1.x, p0.y, p0.z), 0),
            weight.x),
        mix(texelFetch(sampler, ivec3(p0.x, p1.y, p0.z), 0),
            texelFetch(sampler, ivec3(p1.x, p1.y, p0.z), 0),
            weight.x),
        weight.y);
    vec4 z1 = mix(
        mix(texelFetch(sampler, ivec3(p0.x, p0.y, p1.z), 0),
            texelFetch(sampler, ivec3(p1.x, p0.y, p1.z), 0),
            weight.x),
        mix(texelFetch(sampler, ivec3(p0.x, p1.y, p1.z), 0),
            texelFetch(sampler, ivec3(p1.x, p1.y, p1.z), 0),
            weight.x),
        weight.y);
    return mix(z0, z1, weight.z);
}

vec4 SampleScatteringTexture(sampler3D sampler, vec3 uv)
{
    return manual_scattering_filtering != 0
        ? SampleFloatTexture3DLinear(sampler, uv)
        : texture(sampler, uv);
}

DimensionlessSpectrum GetTransmittanceToTopAtmosphereBoundary(
    IN(AtmosphereParameters) parameters,
    IN(TransmittanceTexture) texture_sampler,
    Length r,
    Number mu)
{
    vec2 uv = GetTransmittanceTextureUvFromRMu(parameters, r, mu);
    return DimensionlessSpectrum(SampleFloatTexture2D(texture_sampler, uv));
}

DimensionlessSpectrum GetTransmittance(
    IN(AtmosphereParameters) parameters,
    IN(TransmittanceTexture) texture_sampler,
    Length r,
    Number mu,
    Length d,
    bool ray_r_mu_intersects_ground)
{
    Length r_d = ClampRadius(
        parameters,
        sqrt(d * d + 2.0 * r * mu * d + r * r));
    Number mu_d = ClampCosine((r * mu + d) / r_d);

    if (ray_r_mu_intersects_ground)
    {
        return min(
            GetTransmittanceToTopAtmosphereBoundary(
                parameters, texture_sampler, r_d, -mu_d) /
            GetTransmittanceToTopAtmosphereBoundary(
                parameters, texture_sampler, r, -mu),
            DimensionlessSpectrum(1.0));
    }

    return min(
        GetTransmittanceToTopAtmosphereBoundary(
            parameters, texture_sampler, r, mu) /
        GetTransmittanceToTopAtmosphereBoundary(
            parameters, texture_sampler, r_d, mu_d),
        DimensionlessSpectrum(1.0));
}

DimensionlessSpectrum GetTransmittanceToSun(
    IN(AtmosphereParameters) parameters,
    IN(TransmittanceTexture) texture_sampler,
    Length r,
    Number mu_s)
{
    Number sin_theta_h = parameters.bottom_radius / r;
    Number cos_theta_h =
        -SafeSqrt(1.0 - sin_theta_h * sin_theta_h);
    return GetTransmittanceToTopAtmosphereBoundary(
               parameters, texture_sampler, r, mu_s) *
        smoothstep(
            -sin_theta_h * parameters.sun_angular_radius,
            sin_theta_h * parameters.sun_angular_radius,
            mu_s - cos_theta_h);
}

vec2 GetIrradianceTextureUvFromRMuS(
    IN(AtmosphereParameters) parameters,
    Length r,
    Number mu_s)
{
    Number x_r =
        (r - parameters.bottom_radius) /
        (parameters.top_radius - parameters.bottom_radius);
    Number x_mu_s = mu_s * 0.5 + 0.5;
    return vec2(
        GetTextureCoordFromUnitRange(
            x_mu_s, IRRADIANCE_TEXTURE_WIDTH),
        GetTextureCoordFromUnitRange(
            x_r, IRRADIANCE_TEXTURE_HEIGHT));
}

IrradianceSpectrum GetIrradiance(
    IN(AtmosphereParameters) parameters,
    IN(IrradianceTexture) texture_sampler,
    Length r,
    Number mu_s)
{
    vec2 uv =
        GetIrradianceTextureUvFromRMuS(parameters, r, mu_s);
    return IrradianceSpectrum(
        SampleFloatTexture2D(texture_sampler, uv));
}

IrradianceSpectrum GetSunAndSkyIrradiance(
    IN(AtmosphereParameters) parameters,
    IN(TransmittanceTexture) transmittance_sampler,
    IN(IrradianceTexture) irradiance_sampler,
    IN(Position) point,
    IN(Direction) normal,
    IN(Direction) light_direction,
    OUT(IrradianceSpectrum) sky_irradiance)
{
    Length r = length(point);
    Number mu_s = dot(point, light_direction) / r;
    sky_irradiance =
        GetIrradiance(parameters, irradiance_sampler, r, mu_s) *
        (1.0 + dot(normal, point) / r) * 0.5;
    return parameters.solar_irradiance *
        GetTransmittanceToSun(
            parameters, transmittance_sampler, r, mu_s) *
        max(dot(normal, light_direction), 0.0);
}

InverseSolidAngle RayleighPhaseFunction(Number nu)
{
    InverseSolidAngle k = 3.0 / (16.0 * PI * sr);
    return k * (1.0 + nu * nu);
}

InverseSolidAngle MiePhaseFunction(Number g, Number nu)
{
    InverseSolidAngle k =
        3.0 / (8.0 * PI * sr) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + nu * nu) /
        pow(1.0 + g * g - 2.0 * g * nu, 1.5);
}

vec4 GetScatteringTextureUvwzFromRMuMuSNu(
    IN(AtmosphereParameters) parameters,
    Length r,
    Number mu,
    Number mu_s,
    Number nu,
    bool ray_r_mu_intersects_ground)
{
    Length H = sqrt(
        parameters.top_radius * parameters.top_radius -
        parameters.bottom_radius * parameters.bottom_radius);
    Length rho = SafeSqrt(
        r * r - parameters.bottom_radius * parameters.bottom_radius);
    Number u_r =
        GetTextureCoordFromUnitRange(rho / H, SCATTERING_TEXTURE_R_SIZE);

    Length r_mu = r * mu;
    Area discriminant =
        r_mu * r_mu - r * r +
        parameters.bottom_radius * parameters.bottom_radius;
    Number u_mu;
    if (ray_r_mu_intersects_ground)
    {
        Length d = -r_mu - SafeSqrt(discriminant);
        Length d_min = r - parameters.bottom_radius;
        Length d_max = rho;
        u_mu = 0.5 - 0.5 * GetTextureCoordFromUnitRange(
            d_max == d_min ? 0.0 : (d - d_min) / (d_max - d_min),
            SCATTERING_TEXTURE_MU_SIZE / 2);
    }
    else
    {
        Length d = -r_mu + SafeSqrt(discriminant + H * H);
        Length d_min = parameters.top_radius - r;
        Length d_max = rho + H;
        u_mu = 0.5 + 0.5 * GetTextureCoordFromUnitRange(
            (d - d_min) / (d_max - d_min),
            SCATTERING_TEXTURE_MU_SIZE / 2);
    }

    Length d = DistanceToTopAtmosphereBoundary(
        parameters, parameters.bottom_radius, mu_s);
    Length d_min = parameters.top_radius - parameters.bottom_radius;
    Length d_max = H;
    Number a = (d - d_min) / (d_max - d_min);
    Length D = DistanceToTopAtmosphereBoundary(
        parameters, parameters.bottom_radius, parameters.mu_s_min);
    Number A = (D - d_min) / (d_max - d_min);
    Number u_mu_s = GetTextureCoordFromUnitRange(
        max(1.0 - a / A, 0.0) / (1.0 + a),
        SCATTERING_TEXTURE_MU_S_SIZE);

    Number u_nu = (nu + 1.0) / 2.0;
    return vec4(u_nu, u_mu_s, u_mu, u_r);
}

vec3 GetExtrapolatedSingleMieScattering(
    IN(AtmosphereParameters) parameters,
    IN(vec4) scattering)
{
    if (scattering.r <= 0.0)
        return vec3(0.0);

    return scattering.rgb * scattering.a / scattering.r *
        (parameters.rayleigh_scattering.r / parameters.mie_scattering.r) *
        (parameters.mie_scattering / parameters.rayleigh_scattering);
}

IrradianceSpectrum GetCombinedScattering(
    IN(AtmosphereParameters) parameters,
    IN(ReducedScatteringTexture) scattering_sampler,
    IN(ReducedScatteringTexture) single_mie_sampler,
    Length r,
    Number mu,
    Number mu_s,
    Number nu,
    bool ray_r_mu_intersects_ground,
    OUT(IrradianceSpectrum) single_mie_scattering)
{
    vec4 uvwz = GetScatteringTextureUvwzFromRMuMuSNu(
        parameters,
        r,
        mu,
        mu_s,
        nu,
        ray_r_mu_intersects_ground);
    Number tex_coord_x =
        uvwz.x * Number(SCATTERING_TEXTURE_NU_SIZE - 1);
    Number tex_x = floor(tex_coord_x);
    Number lerp = tex_coord_x - tex_x;
    vec3 uvw0 = vec3(
        (tex_x + uvwz.y) / Number(SCATTERING_TEXTURE_NU_SIZE),
        uvwz.z,
        uvwz.w);
    vec3 uvw1 = vec3(
        (tex_x + 1.0 + uvwz.y) / Number(SCATTERING_TEXTURE_NU_SIZE),
        uvwz.z,
        uvwz.w);

    vec4 scattering0 = SampleScatteringTexture(scattering_sampler, uvw0);
    vec4 scattering1 = SampleScatteringTexture(scattering_sampler, uvw1);
    vec4 combined_scattering =
        scattering0 * (1.0 - lerp) + scattering1 * lerp;
    IrradianceSpectrum scattering =
        IrradianceSpectrum(combined_scattering);

    if (combined_scattering_textures != 0)
    {
        single_mie_scattering =
            GetExtrapolatedSingleMieScattering(
                parameters, combined_scattering);
    }
    else
    {
        single_mie_scattering = IrradianceSpectrum(
            SampleScatteringTexture(single_mie_sampler, uvw0) *
                (1.0 - lerp) +
            SampleScatteringTexture(single_mie_sampler, uvw1) * lerp);
    }

    return scattering;
}

Area GetRaySphereDiscriminant(
    Position origin,
    Direction ray,
    Length radius)
{
    Length projected_distance = dot(origin, ray);
    Position perpendicular =
        origin - ray * projected_distance;
    return radius * radius - dot(perpendicular, perpendicular);
}

RadianceSpectrum GetSkyRadiance(
    IN(AtmosphereParameters) parameters,
    IN(TransmittanceTexture) transmittance_sampler,
    IN(ReducedScatteringTexture) scattering_sampler,
    IN(ReducedScatteringTexture) single_mie_sampler,
    Position camera_position,
    IN(Direction) ray,
    Length shadow_length,
    IN(Direction) light_direction,
    OUT(DimensionlessSpectrum) transmittance)
{
    Length r = length(camera_position);
    Length rmu = dot(camera_position, ray);
    Area discriminant = GetRaySphereDiscriminant(
        camera_position,
        ray,
        parameters.top_radius);
    if (discriminant < 0.0 * m2)
    {
        transmittance = DimensionlessSpectrum(1.0);
        return RadianceSpectrum(
            0.0 * watt_per_square_meter_per_sr_per_nm);
    }
    Length distance_to_top_atmosphere_boundary = -rmu -
        sqrt(discriminant);
    if (distance_to_top_atmosphere_boundary > 0.0 * m)
    {
        camera_position += ray * distance_to_top_atmosphere_boundary;
        r = parameters.top_radius;
        rmu += distance_to_top_atmosphere_boundary;
    }
    else if (r > parameters.top_radius)
    {
        transmittance = DimensionlessSpectrum(1.0);
        return RadianceSpectrum(
            0.0 * watt_per_square_meter_per_sr_per_nm);
    }

    Number mu = rmu / r;
    Number mu_s = dot(camera_position, light_direction) / r;
    Number nu = dot(ray, light_direction);
    bool ray_r_mu_intersects_ground =
        RayIntersectsGround(parameters, r, mu);

    transmittance = ray_r_mu_intersects_ground
        ? DimensionlessSpectrum(0.0)
        : GetTransmittanceToTopAtmosphereBoundary(
              parameters, transmittance_sampler, r, mu);
    IrradianceSpectrum single_mie_scattering;
    IrradianceSpectrum scattering;
    if (shadow_length == 0.0 * m)
    {
        scattering = GetCombinedScattering(
            parameters,
            scattering_sampler,
            single_mie_sampler,
            r,
            mu,
            mu_s,
            nu,
            ray_r_mu_intersects_ground,
            single_mie_scattering);
    }
    else
    {
        Length d = shadow_length;
        Length r_p = ClampRadius(
            parameters,
            sqrt(d * d + 2.0 * r * mu * d + r * r));
        Number mu_p = (r * mu + d) / r_p;
        Number mu_s_p = (r * mu_s + d * nu) / r_p;

        scattering = GetCombinedScattering(
            parameters,
            scattering_sampler,
            single_mie_sampler,
            r_p,
            mu_p,
            mu_s_p,
            nu,
            ray_r_mu_intersects_ground,
            single_mie_scattering);
        DimensionlessSpectrum shadow_transmittance = GetTransmittance(
            parameters,
            transmittance_sampler,
            r,
            mu,
            shadow_length,
            ray_r_mu_intersects_ground);
        scattering *= shadow_transmittance;
        single_mie_scattering *= shadow_transmittance;
    }

    return scattering * RayleighPhaseFunction(nu) +
        single_mie_scattering *
            MiePhaseFunction(parameters.mie_phase_function_g, nu);
}

RadianceSpectrum GetSkyRadianceToPoint(
    IN(AtmosphereParameters) parameters,
    IN(TransmittanceTexture) transmittance_sampler,
    IN(ReducedScatteringTexture) scattering_sampler,
    IN(ReducedScatteringTexture) single_mie_sampler,
    Position camera_position,
    IN(Position) point,
    Length shadow_length,
    IN(Direction) light_direction,
    OUT(DimensionlessSpectrum) transmittance)
{
    Direction ray = normalize(point - camera_position);
    Length r = length(camera_position);
    Length rmu = dot(camera_position, ray);
    Area discriminant = GetRaySphereDiscriminant(
        camera_position,
        ray,
        parameters.top_radius);
    Length distance_to_top_atmosphere_boundary = -rmu -
        SafeSqrt(discriminant);
    if (distance_to_top_atmosphere_boundary > 0.0 * m)
    {
        camera_position += ray * distance_to_top_atmosphere_boundary;
        r = parameters.top_radius;
        rmu += distance_to_top_atmosphere_boundary;
    }

    Number mu = rmu / r;
    Number mu_s = dot(camera_position, light_direction) / r;
    Number nu = dot(ray, light_direction);
    Length d = length(point - camera_position);
    bool ray_r_mu_intersects_ground =
        RayIntersectsGround(parameters, r, mu);

    transmittance = GetTransmittance(
        parameters,
        transmittance_sampler,
        r,
        mu,
        d,
        ray_r_mu_intersects_ground);

    IrradianceSpectrum single_mie_scattering;
    IrradianceSpectrum scattering = GetCombinedScattering(
        parameters,
        scattering_sampler,
        single_mie_sampler,
        r,
        mu,
        mu_s,
        nu,
        ray_r_mu_intersects_ground,
        single_mie_scattering);

    d = max(d - shadow_length, 0.0 * m);
    Length r_p = ClampRadius(
        parameters,
        sqrt(d * d + 2.0 * r * mu * d + r * r));
    Number mu_p = (r * mu + d) / r_p;
    Number mu_s_p = (r * mu_s + d * nu) / r_p;

    IrradianceSpectrum single_mie_scattering_p;
    IrradianceSpectrum scattering_p = GetCombinedScattering(
        parameters,
        scattering_sampler,
        single_mie_sampler,
        r_p,
        mu_p,
        mu_s_p,
        nu,
        ray_r_mu_intersects_ground,
        single_mie_scattering_p);

    DimensionlessSpectrum shadow_transmittance = transmittance;
    if (shadow_length > 0.0 * m)
    {
        shadow_transmittance = GetTransmittance(
            parameters,
            transmittance_sampler,
            r,
            mu,
            d,
            ray_r_mu_intersects_ground);
    }
    scattering -= shadow_transmittance * scattering_p;
    single_mie_scattering -=
        shadow_transmittance * single_mie_scattering_p;
    if (combined_scattering_textures != 0)
    {
        single_mie_scattering = GetExtrapolatedSingleMieScattering(
            parameters,
            vec4(scattering, single_mie_scattering.r));
    }

    single_mie_scattering *= smoothstep(0.0, 0.01, mu_s);
    return scattering * RayleighPhaseFunction(nu) +
        single_mie_scattering *
            MiePhaseFunction(parameters.mie_phase_function_g, nu);
}

vec3 GetSkyLuminance(
    Position camera_position,
    Direction ray,
    Length shadow_length,
    Direction light_direction,
    out DimensionlessSpectrum transmittance)
{
    return GetSkyRadiance(
               atmosphere,
               transmittance_texture,
               scattering_texture,
               single_mie_scattering_texture,
               camera_position,
               ray,
               shadow_length,
               light_direction,
               transmittance) *
        sky_spectral_radiance_to_luminance;
}

vec3 GetSkyLuminanceToPoint(
    Position camera_position,
    Position point,
    Length shadow_length,
    Direction light_direction,
    out DimensionlessSpectrum transmittance)
{
    return GetSkyRadianceToPoint(
               atmosphere,
               transmittance_texture,
               scattering_texture,
               single_mie_scattering_texture,
               camera_position,
               point,
               shadow_length,
               light_direction,
               transmittance) *
        sky_spectral_radiance_to_luminance;
}

vec2 RaySphereIntersections(
    Position origin,
    Direction ray,
    Length radius)
{
    Length b = dot(origin, ray);
    Area discriminant =
        GetRaySphereDiscriminant(origin, ray, radius);
    if (discriminant < 0.0)
        return vec2(-1.0);

    Length root = sqrt(discriminant);
    return vec2(-b - root, -b + root);
}

Length ReconstructSceneDistance(
    float depth,
    float distance_scale)
{
    if (depth >= 1.0)
        return 1.0e30;

    float count = float(depth_partition_count);
    int partition_index = clamp(
        int(floor((1.0 - depth) * count)),
        0,
        depth_partition_count - 1);
    float range_min =
        1.0 - float(partition_index + 1) / count;
    float local_depth =
        clamp((depth - range_min) * count, 0.0, 1.0);
    float ndc_z = local_depth * 2.0 - 1.0;

    vec2 near_far = texelFetch(
        depth_partition_texture,
        ivec2(partition_index, 0),
        0).rg;
    float denominator =
        near_far.y + near_far.x -
        ndc_z * (near_far.y - near_far.x);
    float view_z =
        2.0 * near_far.x * near_far.y /
        denominator;
    return view_z * distance_scale;
}

Length GetSceneDistance()
{
    vec2 uv =
        (gl_FragCoord.xy - viewport_origin) / viewport_size;
    float depth = texture(scene_depth_texture, uv).r;
    Direction ray_view = normalize(view_ray_view);
    float model_units_per_km =
        length(view_ray) / max(length(view_ray_view), 1.0e-6);
    float distance_scale =
        model_units_per_km / max(-ray_view.z, 1.0e-6);
    return ReconstructSceneDistance(depth, distance_scale);
}

bool IsVisibleSurfaceNearby()
{
    ivec2 size = textureSize(surface_id_texture, 0);
    ivec2 pixel = clamp(
        ivec2(gl_FragCoord.xy - viewport_origin),
        ivec2(0),
        size - ivec2(1));
    float center_alpha =
        texelFetch(surface_id_texture, pixel, 0).a;
    if (center_alpha >= 0.9999)
        return false;
    if (abs(-center_alpha - surface_body_id) < 0.25)
        return true;

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            ivec2 sample_pixel = clamp(
                pixel + ivec2(x, y),
                ivec2(0),
                size - ivec2(1));
            float visible_surface_id =
                -texelFetch(
                    surface_id_texture,
                    sample_pixel,
                    0).a;
            if (abs(visible_surface_id - surface_body_id) < 0.25)
                return true;
        }
    }
    return false;
}

Number GetEclipseShadowVisibility(Position position);
vec3 GetEclipseDirectLuminanceLoss(
    Position origin,
    Direction ray,
    Length distance);

vec2 GetCloudTextureUv(Direction normal)
{
    return vec2(
        cloud_texture_offset -
            atan(normal.z, normal.x) / (2.0 * PI),
        0.5 - asin(clamp(normal.y, -1.0, 1.0)) / PI);
}

vec4 SampleCloudTexture(
    sampler2D texture_sampler,
    vec2 texture_uv)
{
    vec2 texture_dx = dFdx(texture_uv);
    vec2 texture_dy = dFdy(texture_uv);
    texture_dx.x -= round(texture_dx.x);
    texture_dy.x -= round(texture_dy.x);
    return textureGrad(
        texture_sampler,
        texture_uv,
        texture_dx,
        texture_dy);
}

vec4 SampleCloud(vec2 texture_uv)
{
    vec4 sample_value =
        SampleCloudTexture(cloud_texture, texture_uv);
    float density = cloud_texture_has_alpha != 0
        ? sample_value.a
        : sample_value.r;
    return vec4(sample_value.rgb, density);
}

Direction GetCloudNormal(
    Direction geometric_normal,
    vec2 texture_uv)
{
    if (render_cloud_normals == 0)
        return geometric_normal;

    vec4 encoded =
        SampleCloudTexture(cloud_normal_texture, texture_uv);
    vec3 tangent_normal;
    if (cloud_normal_texture_dxt5 != 0)
    {
        tangent_normal.xy = encoded.ag * 2.0 - vec2(1.0);
        tangent_normal.z = sqrt(max(
            1.0 - dot(tangent_normal.xy, tangent_normal.xy),
            0.0));
    }
    else
    {
        tangent_normal = encoded.xyz * 2.0 - vec3(1.0);
    }

    Length equatorial_length = length(geometric_normal.xz);
    Direction tangent = equatorial_length > 1.0e-6
        ? vec3(
              geometric_normal.z / equatorial_length,
              0.0,
              -geometric_normal.x / equatorial_length)
        : vec3(1.0, 0.0, 0.0);
    Direction bitangent = -cross(geometric_normal, tangent);
    return normalize(
        tangent * tangent_normal.x +
        bitangent * tangent_normal.y +
        geometric_normal * tangent_normal.z);
}

bool GetCloudIntersection(
    Position origin,
    Direction ray,
    Length surface_distance,
    out Length cloud_distance)
{
    cloud_distance = 0.0;
    Length thickness =
        (cloud_radius - atmosphere.bottom_radius) * 0.5;
    if (render_clouds == 0 || thickness <= 0.0)
        return false;

    vec2 outer_intersections =
        RaySphereIntersections(origin, ray, cloud_radius);
    if (outer_intersections.y < 0.0 ||
        outer_intersections.x > outer_intersections.y)
    {
        return false;
    }

    if (outer_intersections.x < 0.0 &&
        surface_distance < outer_intersections.y)
    {
        return false;
    }

    cloud_distance = outer_intersections.x < 0.0
        ? outer_intersections.y
        : outer_intersections.x;
    if (cloud_distance >= surface_distance)
        return false;

    return true;
}

vec4 GetCloudColor(
    Position origin,
    Direction ray,
    Length surface_distance)
{
    Length cloud_distance;
    if (!GetCloudIntersection(
            origin, ray, surface_distance, cloud_distance))
    {
        return vec4(0.0);
    }

    Position cloud_point = origin + ray * cloud_distance;
    Direction geometric_normal = normalize(cloud_point);
    vec2 texture_uv = GetCloudTextureUv(geometric_normal);
    vec4 cloud_sample = SampleCloud(texture_uv);
    cloud_sample.a = clamp(cloud_sample.a, 0.0, 1.0);
    float alpha = cloud_sample.a;
    if (alpha <= 0.0)
        return vec4(0.0);

    vec3 cloud_transmittance;
    vec3 front_scattering =
        GetSkyLuminanceToPoint(
            origin,
            cloud_point,
            0.0,
            sun_direction,
            cloud_transmittance) -
        GetEclipseDirectLuminanceLoss(
            origin,
            ray,
            cloud_distance);

    vec3 sky_irradiance;
    Direction cloud_normal =
        GetCloudNormal(geometric_normal, texture_uv);
    vec3 sun_irradiance = GetSunAndSkyIrradiance(
        atmosphere,
        transmittance_texture,
        irradiance_texture,
        cloud_point,
        cloud_normal,
        sun_direction,
        sky_irradiance);
    sun_irradiance *= GetEclipseShadowVisibility(cloud_point);

    vec3 cloud_radiance =
        front_scattering +
        cloud_transmittance * cloud_sample.rgb *
            (sun_irradiance * sun_spectral_radiance_to_luminance +
             sky_irradiance * sky_spectral_radiance_to_luminance) / PI;
    return vec4(cloud_radiance, alpha);
}

DimensionlessSpectrum GetSkyTransmittance(
    Position camera_position,
    Direction ray,
    Length distance)
{
    Length r = length(camera_position);
    Length rmu = dot(camera_position, ray);
    Area discriminant = GetRaySphereDiscriminant(
        camera_position,
        ray,
        atmosphere.top_radius);
    if (discriminant < 0.0)
        return vec3(1.0);

    Length atmosphere_entry = -rmu - SafeSqrt(discriminant);
    if (atmosphere_entry > 0.0)
    {
        camera_position += ray * atmosphere_entry;
        distance = max(distance - atmosphere_entry, 0.0);
        r = atmosphere.top_radius;
        rmu += atmosphere_entry;
    }
    else if (r > atmosphere.top_radius)
    {
        return vec3(1.0);
    }

    Number mu = rmu / r;
    bool ray_intersects_ground =
        RayIntersectsGround(atmosphere, r, mu);
    if (distance >= 1.0e29)
    {
        return ray_intersects_ground
            ? vec3(0.0)
            : GetTransmittanceToTopAtmosphereBoundary(
                  atmosphere,
                  transmittance_texture,
                  r,
                  mu);
    }

    return GetTransmittance(
        atmosphere,
        transmittance_texture,
        r,
        mu,
        distance,
        ray_intersects_ground);
}

Number GetLayerDensity(
    IN(DensityProfileLayer) layer,
    Length altitude)
{
    return clamp(
        layer.exp_term * exp(layer.exp_scale * altitude) +
            layer.linear_term * altitude +
            layer.constant_term,
        0.0,
        1.0);
}

Number GetProfileDensity(
    IN(DensityProfileLayer) layer0,
    IN(DensityProfileLayer) layer1,
    Length altitude)
{
    return altitude < layer0.width
        ? GetLayerDensity(layer0, altitude)
        : GetLayerDensity(layer1, altitude);
}

Number GetEclipseShadowDepth(
    int shadow_index,
    Position position)
{
    vec2 shadow_center = vec2(
        dot(
            vec4(position, 1.0),
            eclipse_shadow_tex_gen_s[shadow_index]),
        dot(
            vec4(position, 1.0),
            eclipse_shadow_tex_gen_t[shadow_index])) -
        vec2(0.5);
    return clamp(
        (2.0 * length(shadow_center) - 1.0) *
            eclipse_shadow_falloff[shadow_index],
        0.0,
        eclipse_shadow_max_depth[shadow_index]);
}

Number GetEclipseShadowVisibility(Position position)
{
    Number visibility = 1.0;
    for (int i = 0; i < MAX_ECLIPSE_SHADOWS; ++i)
    {
        if (i >= eclipse_shadow_count)
            break;

        visibility *= 1.0 - GetEclipseShadowDepth(i, position);
    }
    return visibility;
}

bool GetEclipseShadowInterval(
    int shadow_index,
    Position origin,
    Direction ray,
    Length minimum_distance,
    Length maximum_distance,
    out Length interval_start,
    out Length interval_end)
{
    Length segment_length =
        maximum_distance - minimum_distance;
    Position segment_origin =
        origin + ray * minimum_distance;
    vec2 shadow_origin = vec2(
        dot(
            vec4(segment_origin, 1.0),
            eclipse_shadow_tex_gen_s[shadow_index]),
        dot(
            vec4(segment_origin, 1.0),
            eclipse_shadow_tex_gen_t[shadow_index])) -
        vec2(0.5);
    vec2 shadow_direction = vec2(
        dot(
            vec4(ray, 0.0),
            eclipse_shadow_tex_gen_s[shadow_index]),
        dot(
            vec4(ray, 0.0),
            eclipse_shadow_tex_gen_t[shadow_index]));

    Number a = dot(shadow_direction, shadow_direction);
    Number c = dot(shadow_origin, shadow_origin) - 0.25;
    if (a <= 1.0e-12)
    {
        if (c > 0.0)
            return false;

        interval_start = minimum_distance;
        interval_end = maximum_distance;
        return segment_length > 0.0;
    }

    Number b = dot(shadow_origin, shadow_direction);
    Number discriminant = b * b - a * c;
    if (discriminant <= 0.0)
        return false;

    Number root = sqrt(discriminant);
    interval_start =
        minimum_distance +
        max(0.0, (-b - root) / a);
    interval_end =
        minimum_distance +
        min(segment_length, (-b + root) / a);
    return interval_end > interval_start;
}

vec3 GetEclipseDirectLuminanceLoss(
    Position origin,
    Direction ray,
    Length distance)
{
    if (eclipse_shadow_count == 0)
        return vec3(0.0);

    vec2 atmosphere_intersections = RaySphereIntersections(
        origin, ray, atmosphere.top_radius);
    Length start_distance = max(atmosphere_intersections.x, 0.0);
    Length end_distance = min(atmosphere_intersections.y, distance);
    if (end_distance <= start_distance)
        return vec3(0.0);

    Length interval_starts[MAX_ECLIPSE_SHADOWS];
    Length interval_ends[MAX_ECLIPSE_SHADOWS];
    int interval_count = 0;
    for (int shadow_index = 0;
         shadow_index < MAX_ECLIPSE_SHADOWS;
         ++shadow_index)
    {
        if (shadow_index >= eclipse_shadow_count)
            break;

        Length interval_start;
        Length interval_end;
        if (GetEclipseShadowInterval(
                shadow_index,
                origin,
                ray,
                start_distance,
                end_distance,
                interval_start,
                interval_end))
        {
            int insertion_index = interval_count;
            for (int interval_index = 0;
                 interval_index < MAX_ECLIPSE_SHADOWS;
                 ++interval_index)
            {
                if (interval_index >= interval_count)
                    break;

                if (interval_start <
                    interval_starts[interval_index])
                {
                    insertion_index = interval_index;
                    break;
                }
            }
            for (int interval_index =
                     MAX_ECLIPSE_SHADOWS - 1;
                 interval_index > 0;
                 --interval_index)
            {
                if (interval_index > insertion_index &&
                    interval_index <= interval_count)
                {
                    interval_starts[interval_index] =
                        interval_starts[interval_index - 1];
                    interval_ends[interval_index] =
                        interval_ends[interval_index - 1];
                }
            }
            interval_starts[insertion_index] = interval_start;
            interval_ends[insertion_index] = interval_end;
            ++interval_count;
        }
    }
    if (interval_count == 0)
        return vec3(0.0);

    Length merged_starts[MAX_ECLIPSE_SHADOWS];
    Length merged_ends[MAX_ECLIPSE_SHADOWS];
    int merged_count = 0;
    for (int interval_index = 0;
         interval_index < MAX_ECLIPSE_SHADOWS;
         ++interval_index)
    {
        if (interval_index >= interval_count)
            break;

        if (merged_count == 0)
        {
            merged_starts[0] = interval_starts[interval_index];
            merged_ends[0] = interval_ends[interval_index];
            merged_count = 1;
            continue;
        }

        int previous_index = merged_count - 1;
        if (interval_starts[interval_index] <=
            merged_ends[previous_index])
        {
            merged_ends[previous_index] =
                max(
                    merged_ends[previous_index],
                    interval_ends[interval_index]);
        }
        else
        {
            merged_starts[merged_count] =
                interval_starts[interval_index];
            merged_ends[merged_count] =
                interval_ends[interval_index];
            ++merged_count;
        }
    }

    Position atmosphere_start =
        origin + ray * start_distance;
    Length start_r = length(atmosphere_start);
    Number start_mu = dot(atmosphere_start, ray) / start_r;
    bool ray_intersects_ground =
        RayIntersectsGround(atmosphere, start_r, start_mu);

    const int sample_count = 4;
    vec3 lost_radiance = vec3(0.0);
    Number nu = dot(ray, sun_direction);
    vec3 direct_luminance_conversion =
        precomputed_luminance != 0
            ? sun_spectral_radiance_to_luminance
            : sky_spectral_radiance_to_luminance;
    for (int interval_index = 0;
         interval_index < MAX_ECLIPSE_SHADOWS;
         ++interval_index)
    {
        if (interval_index >= merged_count)
            break;

        Length integration_start =
            merged_starts[interval_index];
        Length integration_end =
            merged_ends[interval_index];
        Length step_size =
            (integration_end - integration_start) /
            Number(sample_count);
        vec3 entry_transmittance = GetTransmittance(
            atmosphere,
            transmittance_texture,
            start_r,
            start_mu,
            integration_start - start_distance,
            ray_intersects_ground);
        vec3 optical_depth = vec3(0.0);
        for (int sample_index = 0;
             sample_index < sample_count;
             ++sample_index)
        {
            Length sample_distance =
                integration_start +
                (Number(sample_index) + 0.5) * step_size;
            Position sample_position =
                origin + ray * sample_distance;
            Length r = length(sample_position);
            Length altitude =
                max(r - atmosphere.bottom_radius, 0.0);
            Number rayleigh_density = GetProfileDensity(
                atmosphere.rayleigh_density0,
                atmosphere.rayleigh_density1,
                altitude);
            Number mie_density = GetProfileDensity(
                atmosphere.mie_density0,
                atmosphere.mie_density1,
                altitude);
            Number absorption_density = GetProfileDensity(
                atmosphere.absorption_density0,
                atmosphere.absorption_density1,
                altitude);
            vec3 extinction =
                atmosphere.rayleigh_scattering *
                    rayleigh_density +
                atmosphere.mie_extinction * mie_density +
                atmosphere.absorption_extinction *
                    absorption_density;
            vec3 view_transmittance =
                entry_transmittance *
                exp(-(optical_depth +
                      0.5 * step_size * extinction));
            Number shadow_depth =
                1.0 -
                GetEclipseShadowVisibility(sample_position);
            if (shadow_depth > 0.0)
            {
                Number mu_s =
                    dot(sample_position, sun_direction) / r;
                vec3 direct_source =
                    atmosphere.solar_irradiance *
                    GetTransmittanceToSun(
                        atmosphere,
                        transmittance_texture,
                        r,
                        mu_s) *
                    (
                        atmosphere.rayleigh_scattering *
                            rayleigh_density *
                            RayleighPhaseFunction(nu) +
                        atmosphere.mie_scattering *
                            mie_density *
                            MiePhaseFunction(
                                atmosphere.mie_phase_function_g,
                                nu)
                    );
                lost_radiance +=
                    view_transmittance *
                    direct_source *
                    direct_luminance_conversion *
                    shadow_depth *
                    step_size;
            }
            optical_depth += step_size * extinction;
        }
    }
    return lost_radiance;
}

vec3 GetFiniteSegmentLuminance(
    Position origin,
    Direction ray,
    Length distance,
    out DimensionlessSpectrum transmittance)
{
    vec2 atmosphere_intersections = RaySphereIntersections(
        origin, ray, atmosphere.top_radius);
    Length start_distance = max(atmosphere_intersections.x, 0.0);
    Length end_distance = min(atmosphere_intersections.y, distance);
    if (end_distance <= start_distance)
    {
        transmittance = vec3(1.0);
        return vec3(0.0);
    }

    const int sample_count = 8;
    Length step_size =
        (end_distance - start_distance) / Number(sample_count);
    vec3 optical_depth = vec3(0.0);
    vec3 radiance = vec3(0.0);
    Number nu = dot(ray, sun_direction);
    vec3 direct_luminance_conversion =
        precomputed_luminance != 0
            ? sun_spectral_radiance_to_luminance
            : sky_spectral_radiance_to_luminance;
    for (int i = 0; i < sample_count; ++i)
    {
        Length sample_distance =
            start_distance + (Number(i) + 0.5) * step_size;
        Position sample_position =
            origin + ray * sample_distance;
        Length r = length(sample_position);
        Length altitude = max(r - atmosphere.bottom_radius, 0.0);
        Number rayleigh_density = GetProfileDensity(
            atmosphere.rayleigh_density0,
            atmosphere.rayleigh_density1,
            altitude);
        Number mie_density = GetProfileDensity(
            atmosphere.mie_density0,
            atmosphere.mie_density1,
            altitude);
        Number absorption_density = GetProfileDensity(
            atmosphere.absorption_density0,
            atmosphere.absorption_density1,
            altitude);
        vec3 extinction = (
            atmosphere.rayleigh_scattering * rayleigh_density +
            atmosphere.mie_extinction * mie_density +
            atmosphere.absorption_extinction * absorption_density);
        vec3 view_transmittance =
            exp(-(optical_depth + 0.5 * step_size * extinction));
        Number mu_s = dot(sample_position, sun_direction) / r;
        vec3 direct_source =
            atmosphere.solar_irradiance *
            GetTransmittanceToSun(
                atmosphere,
                transmittance_texture,
                r,
                mu_s) *
            (
                atmosphere.rayleigh_scattering *
                    rayleigh_density *
                    RayleighPhaseFunction(nu) +
                atmosphere.mie_scattering *
                    mie_density *
                    MiePhaseFunction(
                        atmosphere.mie_phase_function_g,
                        nu)
            );
        vec3 diffuse_source =
            GetIrradiance(
                atmosphere,
                irradiance_texture,
                r,
                mu_s) / PI *
            (
                atmosphere.rayleigh_scattering *
                    rayleigh_density +
                atmosphere.mie_scattering *
                    mie_density
            );
        radiance +=
            view_transmittance *
            (
                direct_source * direct_luminance_conversion +
                diffuse_source *
                    sky_spectral_radiance_to_luminance
            ) *
            step_size;
        optical_depth += step_size * extinction;
    }
    transmittance = exp(-optical_depth);
    return max(
        radiance -
            GetEclipseDirectLuminanceLoss(origin, ray, distance),
        vec3(0.0));
}

void main()
{
    vec3 view_direction = normalize(view_ray);
    vec3 transmittance;
    Position camera_position = camera - earth_center;
    Length scene_distance = GetSceneDistance();
    vec2 ground_intersections = RaySphereIntersections(
        camera_position,
        view_direction,
        atmosphere.bottom_radius);
    bool visible_atmosphere_surface = IsVisibleSurfaceNearby();
    if (visible_atmosphere_surface)
    {
        scene_distance = ground_intersections.x >= 0.0
            ? ground_intersections.x
            : 1.0e30;
    }
    vec2 atmosphere_intersections = RaySphereIntersections(
        camera_position, view_direction, atmosphere.top_radius);

    vec3 luminance = vec3(0.0);
    transmittance = vec3(1.0);
    vec4 cloud = vec4(0.0);
    Length atmosphere_entry =
        max(atmosphere_intersections.x, 0.0);
    if (atmosphere_intersections.y > atmosphere_entry &&
        scene_distance >= atmosphere_entry)
    {
        if (render_mode == 0)
        {
            Length cloud_distance;
            float cloud_alpha = 0.0;
            if (GetCloudIntersection(
                    camera_position,
                    view_direction,
                    scene_distance,
                    cloud_distance))
            {
                Position cloud_point =
                    camera_position +
                    view_direction * cloud_distance;
                Direction geometric_normal =
                    normalize(cloud_point);
                vec2 texture_uv =
                    GetCloudTextureUv(geometric_normal);
                cloud_alpha =
                    clamp(SampleCloud(texture_uv).a, 0.0, 1.0);
            }
            transmittance = GetSkyTransmittance(
                camera_position,
                view_direction,
                scene_distance < atmosphere_intersections.y
                    ? scene_distance
                    : 1.0e30);
            fragColor = vec4(
                clamp(
                    transmittance * (1.0 - cloud_alpha),
                    0.0,
                    1.0),
                1.0);
            return;
        }

        if (scene_distance < atmosphere_intersections.y)
        {
            Position point =
                camera_position +
                view_direction * scene_distance;
            luminance = GetSkyLuminanceToPoint(
                camera_position,
                point,
                0.0,
                sun_direction,
                transmittance);
            if (!visible_atmosphere_surface)
            {
                luminance = GetFiniteSegmentLuminance(
                    camera_position,
                    view_direction,
                    scene_distance,
                    transmittance);
            }
        }
        else
        {
            luminance = GetSkyLuminance(
                camera_position,
                view_direction,
                0.0,
                sun_direction,
                transmittance);
        }

        if (visible_atmosphere_surface ||
            scene_distance >= atmosphere_intersections.y)
        {
            luminance -= GetEclipseDirectLuminanceLoss(
                camera_position,
                view_direction,
                min(scene_distance, atmosphere_intersections.y));
        }

        cloud = GetCloudColor(
            camera_position,
            view_direction,
            scene_distance);
    }

    if (render_mode == 0)
    {
        fragColor = vec4(1.0);
        return;
    }

    vec3 scaled_luminance =
        max(
            mix(luminance, cloud.rgb, cloud.a) *
                luminance_scale,
            vec3(0.0));
    fragColor = vec4(scaled_luminance, 1.0);
#ifdef DUAL_SOURCE_BLENDING
    atmosphereTransmission = vec4(
        clamp(
            transmittance * (1.0 - cloud.a),
            0.0,
            1.0),
        1.0);
#endif
}
