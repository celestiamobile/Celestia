in vec2 texCoord;

uniform sampler2D tex;
uniform sampler2D depthTex;

void main()
{
    fragColor = texture(tex, texCoord);
    gl_FragDepth = texture(depthTex, texCoord).r;
}
