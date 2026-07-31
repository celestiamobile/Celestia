in vec2 texCoord;

uniform sampler2D tex;

#ifdef TONE_MAP
// Exposure for tone mapping.
uniform float exposure;
#endif

// Apply the sRGB electro-optical transfer function (IEC 61966-2-1).
// Input is assumed to be linear light; output is gamma-encoded for display.
vec3 linearToSRGB(vec3 c)
{
    // Clamp negatives to avoid undefined pow() behaviour
    c = max(c, vec3(0.0));
    vec3 higher = vec3(1.055) * pow(c, vec3(1.0 / 2.4)) - vec3(0.055);
    vec3 lower  = vec3(12.92) * c;
    // step(edge, x): 0 when x < edge, 1 when x >= edge
    return mix(lower, higher, step(vec3(0.0031308), c));
}

float interleavedGradientNoise(vec2 position)
{
    return fract(52.9829189 * fract(dot(
        position, vec2(0.06711056, 0.00583715))));
}

void main(void)
{
    vec4 color = texture(tex, texCoord);
#ifdef TONE_MAP
    // Exponential tone mapping to roll off HDR highlights.
    vec3 mapped = vec3(1.0) - exp(-exposure * color.rgb);
    vec3 encoded = linearToSRGB(mapped);
#else
    vec3 encoded = linearToSRGB(color.rgb);
#endif
    float dither =
        (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) / 255.0;
    fragColor = vec4(clamp(encoded + dither, 0.0, 1.0), color.a);
}
