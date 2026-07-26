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

struct AtmosphereParameters
{
    vec3 solar_irradiance;
    float sun_angular_radius;
    Length bottom_radius;
    Length top_radius;
    vec3 rayleigh_scattering;
    vec3 mie_scattering;
    Number mie_phase_function_g;
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
uniform int combined_scattering_textures;
uniform int manual_float_filtering;
uniform int depth_partition_count;
uniform int render_clouds;
uniform int cloud_texture_has_alpha;
uniform float surface_body_id;
uniform vec3 camera;
uniform vec3 earth_center;
uniform vec3 sun_direction;
uniform vec3 sky_spectral_radiance_to_luminance;
uniform float luminance_scale;
uniform float cloud_radius;
uniform float cloud_texture_offset;
uniform vec2 viewport_size;
uniform vec2 viewport_origin;
uniform int render_mode;

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

    vec4 scattering0 = texture(scattering_sampler, uvw0);
    vec4 scattering1 = texture(scattering_sampler, uvw1);
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
            texture(single_mie_sampler, uvw0) * (1.0 - lerp) +
            texture(single_mie_sampler, uvw1) * lerp);
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
    // Temporary reference-demo comparison path: the demo currently has
    // "use luminance" disabled, so it tone maps raw spectral radiance.
    return GetSkyRadiance(
               atmosphere,
               transmittance_texture,
               scattering_texture,
               single_mie_scattering_texture,
               camera_position,
               ray,
               shadow_length,
               light_direction,
               transmittance);
}

vec3 GetSkyLuminanceToPoint(
    Position camera_position,
    Position point,
    Length shadow_length,
    Direction light_direction,
    out DimensionlessSpectrum transmittance)
{
    // Temporary reference-demo comparison path: keep raw spectral radiance.
    return GetSkyRadianceToPoint(
               atmosphere,
               transmittance_texture,
               scattering_texture,
               single_mie_scattering_texture,
               camera_position,
               point,
               shadow_length,
               light_direction,
               transmittance);
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

vec2 GetCloudTextureUv(Position position)
{
    Direction normal = normalize(position);
    return vec2(
        cloud_texture_offset -
            atan(normal.z, normal.x) / (2.0 * PI),
        0.5 - asin(clamp(normal.y, -1.0, 1.0)) / PI);
}

vec4 SampleCloud(Position origin, Direction ray, Length distance)
{
    vec4 sample_value = textureLod(
        cloud_texture,
        GetCloudTextureUv(origin + ray * distance),
        0.0);
    float density = cloud_texture_has_alpha != 0
        ? sample_value.a
        : sample_value.r;
    return vec4(sample_value.rgb, density);
}

vec4 GetCloudSample(
    Position origin,
    Direction ray,
    Length surface_distance,
    out Length cloud_distance)
{
    cloud_distance = 0.0;
    Length thickness =
        (cloud_radius - atmosphere.bottom_radius) * 0.5;
    if (render_clouds == 0 || thickness <= 0.0)
        return vec4(0.0);

    vec2 outer_intersections =
        RaySphereIntersections(origin, ray, cloud_radius);
    if (outer_intersections.y < 0.0 ||
        outer_intersections.x > outer_intersections.y)
    {
        return vec4(0.0);
    }

    if (outer_intersections.x < 0.0 &&
        surface_distance < outer_intersections.y)
    {
        return vec4(0.0);
    }

    cloud_distance = outer_intersections.x < 0.0
        ? outer_intersections.y
        : outer_intersections.x;
    if (cloud_distance >= surface_distance)
        return vec4(0.0);

    vec4 cloud_sample =
        SampleCloud(origin, ray, cloud_distance);
    cloud_sample.a = clamp(cloud_sample.a, 0.0, 1.0);
    return cloud_sample;
}

vec4 GetCloudColor(
    Position origin,
    Direction ray,
    Length surface_distance)
{
    Length cloud_distance;
    vec4 cloud_sample = GetCloudSample(
        origin, ray, surface_distance, cloud_distance);
    float alpha = cloud_sample.a;
    if (alpha <= 0.0)
        return vec4(0.0);

    Position cloud_point = origin + ray * cloud_distance;
    vec3 cloud_transmittance;
    vec3 front_scattering = GetSkyLuminanceToPoint(
        origin,
        cloud_point,
        0.0,
        sun_direction,
        cloud_transmittance);

    vec3 sky_irradiance;
    vec3 sun_irradiance = GetSunAndSkyIrradiance(
        atmosphere,
        transmittance_texture,
        irradiance_texture,
        cloud_point,
        normalize(cloud_point),
        sun_direction,
        sky_irradiance);

    vec3 cloud_radiance =
        front_scattering +
        cloud_transmittance * cloud_sample.rgb *
            (sun_irradiance + sky_irradiance) / PI;
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

void main()
{
    vec3 view_direction = normalize(view_ray);
    vec3 transmittance;
    Position camera_position = camera - earth_center;
    Length scene_distance = GetSceneDistance();
    vec2 surface_uv =
        (gl_FragCoord.xy - viewport_origin) / viewport_size;
    float visible_surface_id =
        -texture(surface_id_texture, surface_uv).a;
    // MSAA resolves can blend IDs at silhouettes; validation bounds the
    // resulting differences to antialiased edge pixels.
    if (abs(visible_surface_id - surface_body_id) < 0.25)
    {
        vec2 ground_intersections = RaySphereIntersections(
            camera_position,
            view_direction,
            atmosphere.bottom_radius);
        if (ground_intersections.x >= 0.0)
            scene_distance = ground_intersections.x;
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
            float cloud_alpha = GetCloudSample(
                camera_position,
                view_direction,
                scene_distance,
                cloud_distance).a;
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
}
