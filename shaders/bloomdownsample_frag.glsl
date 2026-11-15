// Adapted from https://github.com/Froyok/Bloom
// MIT License
//
// Copyright (c) 2025 Léna Piquet
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

uniform sampler2D tex;
in vec2 texCoord;

#ifdef BLOOM_PREFILTER
const vec2 offsets[13] = vec2[13](
    vec2(-1.0,  1.0), vec2(1.0,  1.0),
    vec2(-1.0, -1.0), vec2(1.0, -1.0),
    vec2(-2.0,  2.0), vec2(0.0,  2.0), vec2(2.0,  2.0),
    vec2(-2.0,  0.0), vec2(0.0,  0.0), vec2(2.0,  0.0),
    vec2(-2.0, -2.0), vec2(0.0, -2.0), vec2(2.0, -2.0)
);

vec3 safeTexel(ivec2 coord, ivec2 size)
{
    vec3 color = texelFetch(tex, clamp(coord, ivec2(0), size - 1), 0).rgb;
    if (any(isnan(color)))
        return vec3(0.0);
    return clamp(color, vec3(0.0), vec3(65504.0));
}
#endif

vec3 sampleColor(vec2 uv, ivec2 size)
{
    vec3 color = texture(tex, uv).rgb;
#ifdef BLOOM_PREFILTER
    if (any(isnan(color)) || any(isinf(color)))
    {
        // Sanitize before interpolation when the HDR scene has overflowed.
        vec2 position = uv * vec2(size) - 0.5;
        ivec2 base = ivec2(floor(position));
        vec2 phase = fract(position);
        color = mix(mix(safeTexel(base, size), safeTexel(base + ivec2(1, 0), size), phase.x),
                    mix(safeTexel(base + ivec2(0, 1), size), safeTexel(base + ivec2(1, 1), size), phase.x),
                    phase.y);
    }
    color = clamp(color, vec3(0.0), vec3(65504.0));
#else
    // Emulate a bilinear zero border without changing the shared sampler
    // state or requiring GL_CLAMP_TO_BORDER on OpenGL ES.
    vec2 edge = clamp(min(uv * vec2(size) + 0.5,
                          (1.0 - uv) * vec2(size) + 0.5), vec2(0.0), vec2(1.0));
    color *= edge.x * edge.y;
#endif
    return color;
}

#ifdef BLOOM_PREFILTER
vec3 averageBlock(vec3 a, vec3 b, vec3 c, vec3 d)
{
    vec4 brightness = vec4(max(a.r, max(a.g, a.b)), max(b.r, max(b.g, b.b)),
                           max(c.r, max(c.g, c.b)), max(d.r, max(d.g, d.b)));
    vec4 w = 1.0 / (brightness + 1.0);
    return (a * w.x + b * w.y + c * w.z + d * w.w) / dot(w, vec4(1.0));
}
#endif

void main()
{
    ivec2 size = textureSize(tex, 0);
#ifdef BLOOM_PREFILTER
    vec3 samples[13];
    for (int i = 0; i < 13; ++i)
        samples[i] = sampleColor(texCoord + offsets[i] / vec2(size), size);

    vec3 color = 0.5 * averageBlock(samples[0], samples[1], samples[2], samples[3]);
    color += 0.125 * (averageBlock(samples[4], samples[5], samples[8], samples[7])
                   + averageBlock(samples[5], samples[6], samples[9], samples[8])
                   + averageBlock(samples[7], samples[8], samples[11], samples[10])
                   + averageBlock(samples[8], samples[9], samples[12], samples[11]));
#elif defined(BLOOM_COARSE)
    // The wider binomial kernel stabilizes coarse-level variance as a star moves.
    const float w[5] = float[5](0.0625, 0.25, 0.375, 0.25, 0.0625);
    vec3 color = vec3(0.0);
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            color += sampleColor(texCoord + vec2(x, y) / vec2(size), size)
                     * (w[x + 2] * w[y + 2]);
#else
    vec3 color = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            color += sampleColor(texCoord + vec2(x, y) / vec2(size), size)
                     * float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) / 16.0;
#endif
    fragColor = vec4(min(color, vec3(65000.0)), 1.0);
}
