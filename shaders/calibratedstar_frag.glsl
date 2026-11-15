uniform float pointMode;
uniform float pointScale;
in vec3 color;
in float flux;
in float size;
#ifdef BILLBOARD
in vec2 uv;
#endif

// Abramowitz-Stegun approximation; integrating the Gaussian over each pixel
// preserves faint-star flux even when the kernel is only a few pixels wide.
vec4 erfApprox(vec4 x)
{
    vec4 t = 1.0 / (1.0 + 0.3275911 * abs(x));
    vec4 p = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t
              - 0.284496736) * t + 0.254829592) * t;
    return sign(x) * (1.0 - p * exp(-x * x));
}

void main(void)
{
    float fraction = 1.0 / (size * size);
    if (pointMode < 0.5)
    {
#ifdef BILLBOARD
        vec2 coord = uv;
#else
        vec2 coord = gl_PointCoord;
#endif
        vec2 lo = max(vec2(0.0), coord - 0.5 / size);
        vec2 hi = min(vec2(1.0), coord + 0.5 / size);
        vec4 integral = erfApprox((vec4(lo, hi) - 0.5) * 5.656854249);
        vec2 mass = (integral.zw - integral.xy) / (2.0 * 0.999936658);
        fraction = max(0.0, mass.x * mass.y);
    }
    // Irradiance is normalized over 96-DPI logical-pixel area, as in the PSF core.
    fragColor = vec4(color * (flux * pointScale * pointScale * fraction), 1.0);
#ifdef CAMERA_EXPOSURE
    fragColor.rgb *= cameraExposure;
#endif
}
