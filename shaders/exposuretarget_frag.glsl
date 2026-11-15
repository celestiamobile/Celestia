uniform sampler2D meterTex;
uniform float maxEV;
uniform float middleGray;
uniform float cameraEV;

const int HistogramSize = 64;
const float MinimumHistogramEV = -24.0;
const float HighPercentile = 0.99;
const float MinimumHighlightWeight = 0.01;

vec2 encodeEV(float ev)
{
    float encoded = floor((ev + 40.0) * (65535.0 / 64.0) + 0.5);
    float high = floor(encoded / 256.0);
    return vec2(high, encoded - high * 256.0) / 255.0;
}

void main(void)
{
    vec3 statistics = vec3(0.0);
    float peak = 0.0;
    vec2 histogram[HistogramSize];
    for (int i = 0; i < HistogramSize; ++i)
        histogram[i] = vec2(0.0);
    ivec2 size = textureSize(meterTex, 0);
    // Reduce in FP32: mipmap averaging would lose sparse, dim light in FP16.
    for (int y = 0; y < size.y; ++y)
    {
        for (int x = 0; x < size.x; ++x)
        {
            vec4 tile = texelFetch(meterTex, ivec2(x, y), 0);
            peak = max(peak, tile.a);
            if (tile.g > 0.0)
            {
                float physicalEV = tile.r - log2(tile.g) - cameraEV;
                int bin = int(clamp(floor(physicalEV) - MinimumHistogramEV,
                                    0.0, float(HistogramSize - 1)));
                vec2 light = vec2(exp2(tile.r), tile.g);
                histogram[bin] += light;
                statistics.rg += light;
            }
            statistics.b += tile.b;
        }
    }
    float retained = max(0.0, statistics.g - statistics.b * (1.0 - HighPercentile));
    float retainedLight = 0.0;
    for (int i = 0; i < HistogramSize; ++i)
    {
        float count = min(histogram[i].g, retained);
        if (count > 0.0)
        {
            retainedLight += histogram[i].r * (count / histogram[i].g);
            retained -= count;
        }
    }
    if (statistics.b <= 0.0)
    {
        fragColor = vec4(0.0);
        return;
    }
    // Keep a little rejected light; do not normalize by a disappearing retained population.
    float meanLuminance = statistics.g > 0.0
        ? (MinimumHighlightWeight * statistics.r
           + (1.0 - MinimumHighlightWeight) * retainedLight) / statistics.g : 0.0;
    float targetEV = meanLuminance > 0.0
        ? clamp(log2(middleGray) - log2(meanLuminance) + cameraEV, -39.0, maxEV)
        : maxEV;
    // Keep headroom in the measurement attachment, independently of display adaptation.
    float meterEV = peak > 0.0
        ? clamp(cameraEV + log2(8.0 / peak), -39.0, maxEV) : maxEV;
    fragColor = vec4(peak >= 65504.0 ? vec2(0.0) : encodeEV(targetEV), encodeEV(meterEV));
}
