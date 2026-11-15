#ifdef BILLBOARD
layout(location = 0) in vec2 in_Position;
layout(location = 15) in vec3 in_Center;
layout(location = 2) in vec2 in_TexCoord0;
layout(location = 9) in float in_Size;
layout(location = 13) in float in_Flux;
uniform vec2 viewportRcp;
out vec2 uv;
#else
layout(location = 0) in vec3 in_Position;
layout(location = 7) in float in_Size;
layout(location = 9) in float in_Flux;
#endif
layout(location = 8) in vec3 in_Color;

out vec3 color;
out float flux;
out float size;

void main(void)
{
    color = in_Color;
    flux = in_Flux;
    size = max(1.0, ceil(in_Size));
#ifdef BILLBOARD
    uv = in_TexCoord0;
    set_vp(vec4(in_Center, 1.0));
    gl_Position.xy += in_Position * size * viewportRcp * gl_Position.w;
#else
    gl_PointSize = size;
    set_vp(vec4(in_Position, 1.0));
#endif
}
