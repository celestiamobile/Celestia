// FP32 subset of AMD's ffx_a.h, adapted for GLSL 330 / GLSL ES 300.
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Distributed under the MIT license in thirdparty/fsr1/LICENSE.txt.

#ifdef GL_ES
precision highp int;
#endif

#define A_GPU 1
#define A_STATIC
#define AP1 bool
#define AF1 float
#define AF2 vec2
#define AF3 vec3
#define AF4 vec4
#define AU1 uint
#define AU2 uvec2
#define AU4 uvec4
#define ASU2 ivec2
#define AF1_(v) float(v)
#define AF2_(v) vec2(v)
#define AF3_(v) vec3(v)
#define AF4_(v) vec4(v)
#define AF1_AU1(v) uintBitsToFloat(v)
#define AF2_AU2(v) uintBitsToFloat(v)
#define AU1_AF1(v) floatBitsToUint(v)
#define outAU4 out uvec4
#define varAF2(v) vec2 v
#define initAF2(x,y) vec2(x,y)
#define AExp2F1 exp2
#define AFractF1 fract

float ARcpF1(float v) { return 1.0 / v; }
float ASatF1(float v) { return clamp(v, 0.0, 1.0); }
vec3 ASatF3(vec3 v) { return clamp(v, vec3(0.0), vec3(1.0)); }
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
vec3 AMin3F3(vec3 x, vec3 y, vec3 z) { return min(x, min(y, z)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }
vec3 AMax3F3(vec3 x, vec3 y, vec3 z) { return max(x, max(y, z)); }
float AGtZeroF1(float v) { return ASatF1(v * uintBitsToFloat(0x7f800000u)); }
vec3 AGtZeroF3(vec3 v) { return ASatF3(v * uintBitsToFloat(0x7f800000u)); }
float APrxLoRcpF1(float v) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(v)); }
float APrxLoRsqF1(float v) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(v) >> 1u)); }
float APrxMedRcpF1(float v)
{
    float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(v));
    return b * (-b * v + 2.0);
}
vec3 APrxMedRcpF3(vec3 v)
{
    vec3 b = uintBitsToFloat(uvec3(0x7ef19fffu) - floatBitsToUint(v));
    return b * (-b * v + vec3(2.0));
}

// FsrRcasCon also fills an FP16 constant. GLSL 330 has no packHalf2x16.
uint fsrFloatToHalf(float value)
{
    uint bits = floatBitsToUint(value);
    uint sign = (bits >> 16u) & 0x8000u;
    uint exponent = (bits >> 23u) & 0xffu;
    uint mantissa = bits & 0x7fffffu;
    if (exponent == 255u)
        return sign | 0x7c00u | (mantissa == 0u ? 0u : 0x200u);
    int halfExponent = int(exponent) - 112;
    if (halfExponent >= 31)
        return sign | 0x7c00u;
    if (halfExponent < -10)
        return sign;
    uint shift = halfExponent <= 0 ? uint(14 - halfExponent) : 13u;
    if (halfExponent <= 0)
        mantissa |= 0x800000u;
    uint result = (halfExponent <= 0 ? 0u : uint(halfExponent) << 10u) | (mantissa >> shift);
    uint remainder = mantissa & ((1u << shift) - 1u);
    uint halfway = 1u << (shift - 1u);
    result += uint(remainder > halfway || (remainder == halfway && (result & 1u) != 0u));
    return sign | result;
}
uint AU1_AH2_AF2(vec2 value)
{
    return fsrFloatToHalf(value.x) | (fsrFloatToHalf(value.y) << 16u);
}
