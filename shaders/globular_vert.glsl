attribute vec3 in_Position;
attribute vec3 in_TexCoord0; // reuse it for starSize, relStarDensity and colorIndex

uniform sampler2D colorTex;
uniform mat3 m;
uniform vec3 offset;
uniform float brightness;
uniform float pixelWeight;
uniform float scale;

const float clipDistance = 100.0; // observer distance [ly] from globular, where we
                                  // start "morphing" the star-sprite sizes towards
                                  // their physical values

varying vec4 color;

// Convert a display-calibrated (sRGB) brightness value to linear light.
float sRGBToLinear(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

void main(void)
{
    float starSize = in_TexCoord0.s;
    float relStarDensity = in_TexCoord0.t;
    float colorIndex = in_TexCoord0.p;

    vec3 p = m * in_Position.xyz;
    float br = 2.0 * brightness;

    float s = br * starSize * scale;

    // "Morph" the star-sprite sizes at close observer distance such that
    // the overdense globular core is dissolved upon closing in.
    float obsDistanceToStarRatio = length(p + offset) / clipDistance;
    gl_PointSize = s * min(obsDistanceToStarRatio, 1.0);

    // Brightness alpha is display-calibrated; linearize for the linear-light pipeline.
    float linearBr = 2.0 * sRGBToLinear(brightness);
    color = vec4(texture2D(colorTex, vec2(colorIndex, 0.0)).rgb, min(1.0, linearBr * (1.0 - pixelWeight * relStarDensity)));
    set_vp(vec4(p, 1.0));
}
