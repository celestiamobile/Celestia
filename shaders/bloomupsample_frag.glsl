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
uniform vec2 sceneSize;
uniform float zeroBorder;
in vec2 texCoord;

vec3 sampleColor(vec2 uv, vec2 size)
{
    vec3 color = texture(tex, uv).rgb;
    if (zeroBorder > 0.5)
    {
        vec2 edge = clamp(min(uv * size + 0.5, (1.0 - uv) * size + 0.5),
                          vec2(0.0), vec2(1.0));
        color *= edge.x * edge.y;
    }
    return color;
}

void main()
{
    vec2 size = vec2(textureSize(tex, 0));
#ifdef BLOOM_COARSE
    // Accurate reconstruction is cheap on the two smallest levels and rounds the outer halo.
    vec2 position = texCoord * size - 0.5;
    vec2 base = floor(position);
    vec2 phase = position - base;
    vec2 pixelSpan = sceneSize / size;
    vec2 sigma = 0.75 * max(pixelSpan.x, pixelSpan.y) / pixelSpan;
    vec2 weights[8];
    vec2 total = vec2(0.0);
    for (int i = 0; i < 8; ++i)
    {
        vec2 delta = (vec2(i - 3) - phase) / sigma;
        weights[i] = exp(-0.5 * delta * delta);
        total += weights[i];
    }
    vec3 color = vec3(0.0);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            ivec2 coord = ivec2(base) + ivec2(x - 3, y - 3);
            if (zeroBorder <= 0.5 || (all(greaterThanEqual(coord, ivec2(0)))
                                     && all(lessThan(coord, ivec2(size)))))
                color += texelFetch(tex, clamp(coord, ivec2(0), ivec2(size) - 1), 0).rgb
                         * (weights[x].x * weights[y].y);
        }
    color /= total.x * total.y;
#else
    vec2 pixelSpan = sceneSize / size;
    vec2 stepSize = max(pixelSpan.x, pixelSpan.y) / sceneSize;
    // A rotated circular stencil avoids the square footprint of a separable tent.
    const vec2 ring[8] = vec2[8](
        vec2(0.923879533, 0.382683432), vec2(0.382683432, 0.923879533),
        vec2(-0.382683432, 0.923879533), vec2(-0.923879533, 0.382683432),
        vec2(-0.923879533, -0.382683432), vec2(-0.382683432, -0.923879533),
        vec2(0.382683432, -0.923879533), vec2(0.923879533, -0.382683432)
    );
    vec3 color = sampleColor(texCoord, size) * 0.5;
    for (int i = 0; i < 8; ++i)
        color += sampleColor(texCoord + ring[i] * stepSize, size) * 0.0625;
#endif
    fragColor = vec4(min(color, vec3(65000.0)) * 0.5, 0.5);
}
