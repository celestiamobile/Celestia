in vec2 texCoord;

uniform sampler2D tex;
uniform highp sampler2D depthTex;
uniform bool preserveDepth;
uniform vec2 outputSize;
uniform vec2 outputOrigin;

vec4 fsrGather(vec2 p, int channel)
{
    ivec2 size = textureSize(tex, 0);
    ivec2 base = ivec2(floor(p * vec2(size) - vec2(0.5)));
    ivec2 limit = size - ivec2(1);
    // Match textureGather's component order without requiring GLES 3.1.
    return vec4(
        texelFetch(tex, clamp(base + ivec2(0, 1), ivec2(0), limit), 0)[channel],
        texelFetch(tex, clamp(base + ivec2(1, 1), ivec2(0), limit), 0)[channel],
        texelFetch(tex, clamp(base + ivec2(1, 0), ivec2(0), limit), 0)[channel],
        texelFetch(tex, clamp(base, ivec2(0), limit), 0)[channel]);
}

vec4 FsrEasuRF(vec2 p) { return fsrGather(p, 0); }
vec4 FsrEasuGF(vec2 p) { return fsrGather(p, 1); }
vec4 FsrEasuBF(vec2 p) { return fsrGather(p, 2); }

void main(void)
{
    vec2 inputSize = vec2(textureSize(tex, 0));
    uvec4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3,
               inputSize.x, inputSize.y, inputSize.x, inputSize.y,
               outputSize.x, outputSize.y);
    vec3 color;
    FsrEasuF(color, uvec2(gl_FragCoord.xy - outputOrigin), con0, con1, con2, con3);
    fragColor = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);
    if (preserveDepth)
    {
        ivec2 size = textureSize(depthTex, 0);
        ivec2 coord = clamp(ivec2(texCoord * vec2(size)), ivec2(0), size - ivec2(1));
        gl_FragDepth = texelFetch(depthTex, coord, 0).r;
    }
    else
        gl_FragDepth = gl_FragCoord.z;
}
