uniform vec3 color;
in float shade;
in vec3 tailPosition;
in vec3 surfaceNormal;
uniform bool calibratedDust;
uniform mat4 MVPMatrix;
uniform vec3 tailOrigin;
uniform float tailLength;
uniform vec3 cameraTail;
uniform vec3 tailAxis;
uniform float columnScale;
uniform float dustAsymmetry;
uniform int dustLightCount;
uniform vec4 dustLightPosition[4];
uniform vec3 dustLightColor[4];

const float widthSquared = 0.01;
const float nodes[8] = float[8](
    -0.960289856, -0.796666477, -0.525532410, -0.183434642,
     0.183434642,  0.525532410,  0.796666477,  0.960289856);
const float weights[8] = float[8](
    0.101228536, 0.222381034, 0.313706646, 0.362683783,
    0.362683783, 0.313706646, 0.222381034, 0.101228536);

float dustLightColumn(int light, vec3 origin, vec3 direction,
                      float start, float end, float distanceScale)
{
    float span = abs(end - start);
    if (span == 0.0)
        return 0.0;
    bool logarithmic = span > distanceScale;
    float rate = logarithmic ? log(1.0 + span / distanceScale) : 0.0;
    float column = 0.0;
    for (int sampleIndex = 0; sampleIndex < 8; ++sampleIndex)
    {
        float fraction = 0.5 * (1.0 + nodes[sampleIndex]);
        float offset = span * fraction;
        float jacobian = span;
        if (logarithmic)
        {
            // Concentrate samples near the light without increasing the quadrature order.
            float growth = exp(rate * fraction);
            offset = distanceScale * (growth - 1.0);
            jacobian = distanceScale * rate * growth;
        }
        vec3 position = origin + direction * (start + sign(end - start) * offset);
        float axial = dot(position, tailAxis);
        vec3 transverse = position - tailAxis * axial;
        float profile = max(0.0, 1.0 - dot(transverse, transverse) /
                                     max(widthSquared * axial, 1.0e-12));
        float density = profile * profile * (1.0 - axial) * (1.0 - axial);
        vec3 incoming = position - dustLightPosition[light].xyz;
        float distanceSquared = max(dot(incoming, incoming), dustLightPosition[light].w);
        float cosine = clamp(dot(incoming / sqrt(max(dot(incoming, incoming), 1.0e-16)), -direction), -1.0, 1.0);
        float denominator = 1.0 + dustAsymmetry * dustAsymmetry - 2.0 * dustAsymmetry * cosine;
        float phase = (1.0 - dustAsymmetry * dustAsymmetry) /
                      (12.566370614 * denominator * sqrt(denominator));
        column += phase * density * (0.5 * weights[sampleIndex] * jacobian / distanceSquared);
    }
    return column;
}

vec3 exposedDustLight(vec3 light, float column, float exposure, float meterScale)
{
    if (column == 0.0 || columnScale == 0.0 || exposure == 0.0 || meterScale == 0.0)
        return vec3(0.0);
    vec3 scattered = light * column;
    vec3 unexposed = scattered * columnScale;
    vec3 displayed = unexposed * exposure;
    vec3 result = displayed * meterScale;
    for (int channel = 0; channel < 3; ++channel)
    {
        if (light[channel] > 0.0 &&
            (isnan(result[channel]) || isinf(result[channel]) ||
             min(min(scattered[channel], unexposed[channel]),
                 min(displayed[channel], result[channel])) < 1.17549435e-38))
        {
            // Recover only exceptional products; exposure may make an overflowing
            // unexposed column representable, independently for display and metering.
            float value = log2(light[channel]) + log2(column) + log2(columnScale) +
                          log2(exposure) + log2(meterScale);
            result[channel] = value >= log2(65504.0) ? 65504.0 : exp2(value);
        }
    }
    return min(result, vec3(65504.0));
}

vec3 dustRadiance(out vec3 physicalLight)
{
    vec3 direction = normalize(tailPosition - cameraTail);
    float cameraZ = dot(cameraTail, tailAxis);
    vec3 cameraRadial = cameraTail - tailAxis * cameraZ;
    bool inside = cameraZ >= 0.0 && cameraZ <= 1.0 &&
                  dot(cameraRadial, cameraRadial) <= widthSquared * cameraZ;
    float z = dot(tailPosition, tailAxis);
    float dz = dot(direction, tailAxis);
    vec3 radial = tailPosition - tailAxis * z;
    vec3 dr = direction - tailAxis * dz;
    float a = dot(dr, dr);
    float b = 2.0 * dot(radial, dr) - widthSquared * dz;
    float c = dot(radial, radial) - widthSquared * z;
    float nearT = dot(cameraTail - tailPosition, direction);
    float farT = 1.0e8;
    if (a > 1.0e-6)
    {
        float discriminant = b * b - 4.0 * a * c;
        if (discriminant <= 0.0)
            discard;
        float root = sqrt(discriminant);
        nearT = max(nearT, (-b - root) / (2.0 * a));
        farT = min(farT, (-b + root) / (2.0 * a));
    }
    else if (abs(b) > 1.0e-8)
    {
        if (b > 0.0)
            farT = min(farT, -c / b);
        else
            nearT = max(nearT, -c / b);
    }
    else if (c > 0.0)
        discard;
    if (abs(dz) > 1.0e-6)
    {
        float t0 = -z / dz;
        float t1 = (1.0 - z) / dz;
        nearT = max(nearT, min(t0, t1));
        farT = min(farT, max(t0, t1));
    }
    else if (z < 0.0 || z > 1.0)
        discard;
    if (farT <= nearT)
        discard;

    bool useExit = inside;
    if (!inside)
    {
        // A clipped entry surface cannot draw the column; use its exit instead.
        vec3 entry = tailOrigin + tailLength * (tailPosition + direction * nearT);
        vec4 clip = MVPMatrix * vec4(entry, 1.0);
        useExit = clip.z < -clip.w;
    }
    if ((dot(surfaceNormal, direction) > 0.0) != useExit)
        discard;

    float exposure = 1.0;
#ifdef CAMERA_EXPOSURE
    exposure = cameraExposure;
#endif
    vec3 radiance = vec3(0.0);
    physicalLight = vec3(0.0);
    for (int light = 0; light < 4; ++light)
    {
        if (light >= dustLightCount)
            break;
        float closest = clamp(dot(dustLightPosition[light].xyz - tailPosition, direction), nearT, farT);
        vec3 incoming = tailPosition + direction * closest - dustLightPosition[light].xyz;
        float distanceScale = sqrt(max(dot(incoming, incoming), dustLightPosition[light].w));
        float column;
        if (distanceScale >= farT - nearT)
            column = dustLightColumn(light, tailPosition, direction, nearT, farT, distanceScale);
        else
        {
            column = dustLightColumn(light, tailPosition, direction, closest, nearT, distanceScale) +
                     dustLightColumn(light, tailPosition, direction, closest, farT, distanceScale);
        }
        radiance += exposedDustLight(dustLightColor[light], column, exposure, 1.0);
        physicalLight += exposedDustLight(dustLightColor[light], column, exposure, physicalLightScale);
    }
    physicalLight = min(physicalLight, vec3(65504.0));
    return min(radiance, vec3(65504.0));
}

void main(void)
{
    if (calibratedDust)
    {
        vec3 physicalLight;
        fragColor = vec4(dustRadiance(physicalLight), 1.0);
        meterColor = vec4(physicalLight, 1.0);
        return;
    }
    fragColor = vec4(color, shade);
#ifdef CAMERA_EXPOSURE
    fragColor.rgb *= cameraExposure;
#endif
    meterColor = fragColor;
    meterColor.rgb *= physicalLightScale;
}
