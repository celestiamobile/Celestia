layout(location = 0) in vec3 in_Position;
layout(location = 8) in vec4 in_Color;
layout(location = 7) in float in_PointSize;
uniform float rasterScale;
out vec4 color;

void main(void)
{
    gl_PointSize = in_PointSize * rasterScale;
    color = in_Color;
    set_vp(vec4(in_Position, 1.0));
}
