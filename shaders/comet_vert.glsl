layout(location = 0) in vec4 in_Position;
layout(location = 1) in vec3 in_Normal;
in float in_Brightness;

uniform vec3 color;
uniform vec3 viewDir;
uniform float fadeFactor;
uniform vec3 tailOrigin;
uniform float tailLength;

out float shade;
out vec3 tailPosition;
out vec3 surfaceNormal;

void main(void)
{
    shade = abs(dot(viewDir.xyz, in_Normal.xyz) * in_Brightness * fadeFactor);
    tailPosition = (in_Position.xyz - tailOrigin) / tailLength;
    surfaceNormal = in_Normal;
    set_vp(in_Position);
}
