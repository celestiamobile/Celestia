in vec2 texCoord;

uniform sampler2D tex;

void main()
{
    fragColor = texture(tex, texCoord);
}
