in vec2 texCoord;
in float intensity;

uniform sampler2D tex;
uniform highp sampler2D depthTex;
uniform bool preserveDepth;

void main(void)
{
    fragColor = vec4(texture(tex, texCoord).rgb * intensity, 1.0);
    if (preserveDepth)
    {
        ivec2 size = textureSize(depthTex, 0);
        ivec2 coord = clamp(ivec2(texCoord * vec2(size)), ivec2(0), size - ivec2(1));
        gl_FragDepth = texelFetch(depthTex, coord, 0).r;
    }
    else
        gl_FragDepth = gl_FragCoord.z;
}
