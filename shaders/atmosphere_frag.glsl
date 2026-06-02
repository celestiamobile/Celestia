// SPDX-License-Identifier: BSD-3-Clause AND GPL-2.0-or-later
// atmosphere_frag.glsl — per-frame uniforms + main() for the
// fullscreen-quad sky pass. The bulk of the Bruneton precomputed-
// atmospheric-scattering library (definitions, functions, API wrappers)
// is shared with the planet-surface shader and lives in
// shaders/bruneton_lib.glsl; ShaderManager::loadShader concatenates the
// two files before compilation.

// =========================================================================
// Per-frame uniforms and main(). The fragment shader is driven by a
// screen-space quad whose ModelViewMatrix/MVPMatrix has been set to
// identity by the caller; v_ndc carries the ±1 clip-space xy from the
// vertex shader and we reconstruct an object-space view ray from it.
// =========================================================================
in vec2 v_ndc;
uniform mat4  inv_projection;     // clip-space -> view-space (km)
uniform mat4  inv_modelview_km;   // view-space -> object-space (km)
uniform vec3  camera_km;          // observer in object-space (km)
uniform vec3  sun_direction;      // unit vector in object-space
uniform vec3  white_point;        // calibrated from full solar spectrum
uniform float exposure;

void main()
{
    // Reconstruct the view ray in two steps (un-project NDC to a view-space
    // direction, then rotate into object space). Using a direction rather
    // than a two-point subtraction is robust against the depth-partitioned
    // projection Celestia uses for huge scenes, where the inverse of the
    // combined P*MV matrix can become near-singular and yield NaN rays.
    vec4 viewH   = inv_projection * vec4(v_ndc, 1.0, 1.0);
    vec3 viewDir = normalize(viewH.xyz / viewH.w);
    vec3 viewRay = normalize(mat3(inv_modelview_km) * viewDir);

    vec3 transmittance;
    // Demo default: USE_LUMINANCE is undefined -> GetSkyRadiance is real
    // (returns spectral radiance at 680/550/440 nm).
    vec3 radiance = GetSkyRadiance(camera_km, viewRay, 0.0, sun_direction, transmittance);

    if (dot(viewRay, sun_direction) > 0.9999)
        radiance += transmittance * GetSolarRadiance();

    // Bruneton-style exposure tone-map (1 - exp(-x)) — gives a much more
    // vivid horizon glow than Reinhard's x/(1+x) because mid-range values
    // (~0.5 – 2.0 of the exposure scale) get pushed near 1 instead of
    // being compressed under ~0.7.
    //
    // We deliberately do NOT gamma-encode here: Celestia's sRGBViewportEffect
    // does the linear→sRGB conversion as a final post-process. Applying
    // pow(1/2.2) in this shader on top of that would double-encode and wash
    // out the sky (cyan-pale instead of saturated blue).
    vec3 linRGB = radiance * exposure / white_point;
    vec3 mapped = vec3(1.0) - exp(-linRGB);

    float a = clamp(dot(transmittance, vec3(1.0 / 3.0)), 0.0, 1.0);
    fragColor = vec4(mapped, a);
}
