uniform sampler2D tex;
uniform int outputSize;
uniform bool reduce;

void main(void)
{
    vec3 statistics = vec3(0.0);
    float peak = 0.0;
    highp ivec2 size = textureSize(tex, 0);
    highp ivec2 start = ivec2(gl_FragCoord.xy) * size / outputSize;
    highp ivec2 end = (ivec2(gl_FragCoord.xy) + 1) * size / outputSize;
    for (int y = start.y; y < end.y; ++y)
    {
        for (int x = start.x; x < end.x; ++x)
        {
            vec4 value = texelFetch(tex, ivec2(x, y), 0);
            if (reduce)
            {
                statistics.gb += value.gb;
                peak = max(peak, value.a);
                if (value.g > 0.0)
                    statistics.r += exp2(value.r);
                continue;
            }
            vec3 color = value.rgb;
            if (any(isnan(color)))
                continue;
            statistics.b += 1.0;
            color = clamp(color, vec3(0.0), vec3(65504.0));
            peak = max(peak, max(color.r, max(color.g, color.b)));
            // This attachment contains linear light, before PSF soft clipping
            // and decorative glow. Atmosphere contributes even at clear depth.
            float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
            // Empty space must not brighten the objects surrounded by it.
            if (luminance > 0.0)
                statistics.rg += vec2(luminance, 1.0);
        }
    }
    // Cover every source texel; normalize counts so large images still fit in FP16.
    if (!reduce)
        statistics *= min(1.0, 65536.0 / (float(size.x) * float(size.y)));
    // Keep sparse light representable in FP16 without clipping bright tiles.
    float logLight = statistics.r > 0.0 ? log2(statistics.r) : 0.0;
    fragColor = vec4(logLight, statistics.gb, peak);
}
