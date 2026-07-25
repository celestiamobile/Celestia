// Copyright (c) 2017 Eric Bruneton
// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: BSD-3-Clause

// A tiny GLSL-style vec2/vec3/vec4 of double, with the componentwise
// operations and helpers needed to port Eric Bruneton's functions.glsl to CPU.
//
// The precompute runs entirely in double precision for accuracy; the final
// LUTs are converted to float on output. All GLSL "unit" symbols (m, rad, sr,
// nm, watt, ...) are 1.0 in the shader, so we drop them entirely here and work
// with plain doubles.

#pragma once

#include <algorithm>
#include <cmath>

namespace bruneton {

struct dvec2 {
  double x = 0.0, y = 0.0;
  dvec2() = default;
  dvec2(double x_, double y_) : x(x_), y(y_) {}
  explicit dvec2(double s) : x(s), y(s) {}
};

struct dvec3 {
  double x = 0.0, y = 0.0, z = 0.0;
  dvec3() = default;
  dvec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
  explicit dvec3(double s) : x(s), y(s), z(s) {}
  // GLSL-style .r/.g/.b accessors.
  double r() const { return x; }
  double g() const { return y; }
  double b() const { return z; }
};

struct dvec4 {
  double x = 0.0, y = 0.0, z = 0.0, w = 0.0;
  dvec4() = default;
  dvec4(double x_, double y_, double z_, double w_)
      : x(x_), y(y_), z(z_), w(w_) {}
  explicit dvec4(double s) : x(s), y(s), z(s), w(s) {}
  dvec3 xyz() const { return dvec3(x, y, z); }
};

// ---- dvec3 arithmetic ------------------------------------------------------

inline dvec3 operator+(const dvec3& a, const dvec3& b) {
  return dvec3(a.x + b.x, a.y + b.y, a.z + b.z);
}
inline dvec3 operator-(const dvec3& a, const dvec3& b) {
  return dvec3(a.x - b.x, a.y - b.y, a.z - b.z);
}
inline dvec3 operator-(const dvec3& a) { return dvec3(-a.x, -a.y, -a.z); }
inline dvec3 operator*(const dvec3& a, const dvec3& b) {
  return dvec3(a.x * b.x, a.y * b.y, a.z * b.z);
}
inline dvec3 operator/(const dvec3& a, const dvec3& b) {
  return dvec3(a.x / b.x, a.y / b.y, a.z / b.z);
}
inline dvec3 operator*(const dvec3& a, double s) {
  return dvec3(a.x * s, a.y * s, a.z * s);
}
inline dvec3 operator*(double s, const dvec3& a) { return a * s; }
inline dvec3 operator/(const dvec3& a, double s) {
  return dvec3(a.x / s, a.y / s, a.z / s);
}
inline dvec3 operator/(double s, const dvec3& a) {
  return dvec3(s / a.x, s / a.y, s / a.z);
}
inline dvec3& operator+=(dvec3& a, const dvec3& b) {
  a.x += b.x; a.y += b.y; a.z += b.z; return a;
}

// ---- dvec2 arithmetic ------------------------------------------------------

inline dvec2 operator/(const dvec2& a, const dvec2& b) {
  return dvec2(a.x / b.x, a.y / b.y);
}

// ---- scalar helpers (GLSL builtins) ---------------------------------------

inline double clampd(double x, double lo, double hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
inline double mixd(double a, double b, double t) { return a * (1.0 - t) + b * t; }

inline double smoothstepd(double edge0, double edge1, double x) {
  double t = clampd((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// ---- dvec3 componentwise helpers ------------------------------------------

inline dvec3 vexp(const dvec3& a) {
  return dvec3(std::exp(a.x), std::exp(a.y), std::exp(a.z));
}
inline dvec3 vmin(const dvec3& a, const dvec3& b) {
  return dvec3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
}
inline dvec3 vmin(const dvec3& a, double s) {
  return dvec3(std::min(a.x, s), std::min(a.y, s), std::min(a.z, s));
}

// ---- vector functions ------------------------------------------------------

inline double dot(const dvec3& a, const dvec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline double length(const dvec3& a) { return std::sqrt(dot(a, a)); }
inline dvec3 normalize(const dvec3& a) { return a * (1.0 / length(a)); }

}  // namespace bruneton
