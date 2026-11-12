in vec2 texCoord;

uniform sampler2D tex;
uniform highp sampler2D depthTex;
uniform bool preserveDepth;
uniform float sharpness;
uniform vec2 outputOrigin;

vec4 FsrRcasLoadF(ivec2 p)
{
    return texelFetch(tex, clamp(p, ivec2(0), textureSize(tex, 0) - ivec2(1)), 0);
}

void FsrRcasInputF(inout float r, inout float g, inout float b)
{
}

void main(void)
{
    uvec4 con;
    FsrRcasCon(con, sharpness);
    vec3 color;
    FsrRcasF(color.r, color.g, color.b, uvec2(gl_FragCoord.xy - outputOrigin), con);
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
