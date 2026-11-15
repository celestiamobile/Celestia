uniform vec3 color;
in float shade;

void main(void)
{
    fragColor = vec4(color, shade);
#ifdef CAMERA_EXPOSURE
    fragColor.rgb *= cameraExposure;
#endif
}
